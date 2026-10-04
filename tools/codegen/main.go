// Command codegen reads a struct from a pinned sing-box option package via
// go/packages (real type-checking, not regex-over-source) and emits a C++
// header with a matching struct plus nlohmann::json to_json/from_json.
//
// Scope (see Sovereign issues #2, #5): anonymous embedded fields that are
// plain data (no custom MarshalJSON/UnmarshalJSON) of the same package are
// flattened; named struct types of the same package that fields reach are
// generated as C++ structs of their own, in the same header (the requested
// type's TLS options -> OutboundTLSOptions -> OutboundUTLSOptions, ...).
// Types with handwritten adapters (badoption.Duration/Listable/Addr, FwMark,
// CurvePreference) map to them. Anything else that needs real Go JSON
// semantics we can't safely infer here — foreign embedded types, other
// custom (un)marshalers — is emitted as a raw nlohmann::json member with a
// comment, left for the handwritten adapter layer to replace.
package main

import (
	"flag"
	"fmt"
	"go/ast"
	"go/constant"
	"go/types"
	"log"
	"os"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"unicode"

	"golang.org/x/tools/go/packages"
)

type field struct {
	CppName     string
	CppType     string
	JSONName    string
	OmitEmpty   bool
	NeedAdapter bool
	AdapterNote string
	// IntType: the C++ integer type of an integer field (or of an optional
	// one), read through adapters::GetInteger - Go's range and literal rules
	// instead of nlohmann's silent conversions.
	IntType string
	// Variant: a union's variant struct (json:"-" in Go) - merged into the
	// object only when the discriminator picks it, never written by itself.
	Variant bool
}

// unionCase is one variant of a discriminated union: the discriminator's
// value (a C++ literal) and the member holding that variant's fields - none
// when the variant has no fields of its own (salamander obfs).
type unionCase struct {
	Value  string
	typ    types.Type
	Member string
}

// unionInfo is what a type's DescribeSchema says about it through
// schema.DiscriminatedUnion: the discriminator's JSON key and the variants.
type unionInfo struct {
	Key   string
	Field string
	Cases []unionCase
}

var integerTypes = map[string]bool{
	"std::int8_t": true, "std::int16_t": true, "std::int32_t": true, "std::int64_t": true,
	"std::uint8_t": true, "std::uint16_t": true, "std::uint32_t": true, "std::uint64_t": true,
}

// integerOf returns the integer type cppType is, or holds as an optional.
func integerOf(cppType string) string {
	inner := strings.TrimSuffix(strings.TrimPrefix(cppType, "std::optional<"), ">")
	if integerTypes[inner] && (inner == cppType || cppType == "std::optional<"+inner+">") {
		return inner
	}
	return ""
}

func main() {
	src := flag.String("src", "", "path to the sing-box repo root (contains go.mod)")
	pkgPath := flag.String("pkg", "./option", "package pattern to load, relative to -src")
	typeName := flag.String("type", "", "struct name(s) to generate, comma-separated, e.g. AnyTLSOutboundOptions,VLESSOutboundOptions")
	out := flag.String("out", "", "output C++ header path ('-' for stdout)")
	namespace := flag.String("namespace", "sovereign::codegen::option", "C++ namespace for the generated type")
	flag.Parse()

	if *src == "" || *typeName == "" || *out == "" {
		fmt.Fprintln(os.Stderr, "usage: codegen -src <sing-box repo root> -type <StructName> -out <file.h>")
		os.Exit(2)
	}

	cfg := &packages.Config{
		Mode: packages.NeedName | packages.NeedTypes | packages.NeedTypesInfo |
			packages.NeedSyntax | packages.NeedImports | packages.NeedDeps,
		Dir: *src,
	}
	pkgs, err := packages.Load(cfg, *pkgPath)
	if err != nil {
		log.Fatalf("load %s: %v", *pkgPath, err)
	}
	if packages.PrintErrors(pkgs) > 0 {
		log.Fatalf("package %s has load errors (see above)", *pkgPath)
	}
	if len(pkgs) != 1 {
		log.Fatalf("expected exactly one package for pattern %s, got %d", *pkgPath, len(pkgs))
	}
	pkg := pkgs[0]

	// Several types share one header: the structs they reach (the TLS options,
	// the dialer's) are emitted once - two headers each with their own copy
	// couldn't be used together.
	g := &generator{pkg: pkg.Types, info: pkg.TypesInfo, structs: map[string][]field{}, busy: map[string]bool{},
		unions: map[string]*unionInfo{}, schemas: describeSchemas(pkg.Syntax)}
	for _, name := range strings.Split(*typeName, ",") {
		obj := pkg.Types.Scope().Lookup(name)
		if obj == nil {
			log.Fatalf("type %s not found in package %s", name, pkg.PkgPath)
		}
		named, ok := obj.Type().(*types.Named)
		if !ok {
			log.Fatalf("%s is not a named type", name)
		}
		if _, ok := named.Underlying().(*types.Struct); !ok {
			log.Fatalf("%s is not a struct", name)
		}
		if !hasCustomJSONMethods(named) {
			g.ensure(named)
		} else if u := g.findUnion(named); u != nil {
			g.ensureUnion(named, u)
		} else {
			log.Fatalf("%s marshals itself and its schema isn't a discriminated union", name)
		}
	}

	src2 := g.renderHeader(*namespace, *typeName, pkg.PkgPath)

	if *out == "-" {
		fmt.Print(src2)
		return
	}
	if err := os.WriteFile(*out, []byte(src2), 0o644); err != nil {
		log.Fatalf("write %s: %v", *out, err)
	}
	total, adapters := 0, 0
	for _, name := range g.order {
		total += len(g.structs[name])
		adapters += countAdapters(g.structs[name])
	}
	fmt.Fprintf(os.Stderr, "wrote %s (%d structs, %d fields, %d need adapters)\n", *out, len(g.order), total, adapters)
}

// generator collects the structs one header needs: the requested type and,
// recursively, every named struct type of the same package its fields reach
// (OutboundTLSOptions -> OutboundUTLSOptions, OutboundRealityOptions, ...).
// Each becomes a C++ struct of the same name with its own to_json/from_json,
// emitted dependencies first. A type reaching itself would not compile as a
// C++ member - none in the option package does today.
type generator struct {
	pkg     *types.Package
	info    *types.Info
	structs map[string][]field
	order   []string
	busy    map[string]bool
	unions  map[string]*unionInfo
	schemas map[string]*ast.FuncDecl
}

// describeSchemas finds every DescribeSchema method of the package, by the
// name of its receiver's type.
func describeSchemas(files []*ast.File) map[string]*ast.FuncDecl {
	out := map[string]*ast.FuncDecl{}
	for _, file := range files {
		for _, decl := range file.Decls {
			fn, ok := decl.(*ast.FuncDecl)
			if !ok || fn.Recv == nil || fn.Name.Name != "DescribeSchema" || len(fn.Recv.List) != 1 {
				continue
			}
			recv := fn.Recv.List[0].Type
			if star, isStar := recv.(*ast.StarExpr); isStar {
				recv = star.X
			}
			if id, isIdent := recv.(*ast.Ident); isIdent {
				out[id.Name] = fn
			}
		}
	}
	return out
}

// findUnion reads named's DescribeSchema for a schema.DiscriminatedUnion
// call: (builder, "<key>", required, []schema.UnionVariant{{Value: <const>,
// StructType: reflect.TypeFor[T]()}, ...}, ...). Nil when there's none, or
// when a value isn't a constant - then the type stays an adapter's job.
func (g *generator) findUnion(named *types.Named) *unionInfo {
	decl := g.schemas[named.Obj().Name()]
	if decl == nil || decl.Body == nil {
		return nil
	}
	var call *ast.CallExpr
	ast.Inspect(decl.Body, func(n ast.Node) bool {
		if call != nil {
			return false
		}
		c, ok := n.(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := c.Fun.(*ast.SelectorExpr)
		if !ok || sel.Sel.Name != "DiscriminatedUnion" {
			return true
		}
		if id, isIdent := sel.X.(*ast.Ident); isIdent {
			if pn, isPkg := g.info.Uses[id].(*types.PkgName); isPkg && pn.Imported().Path() == "github.com/sagernet/sing-box/schema" {
				call = c
			}
		}
		return call == nil
	})
	if call == nil || len(call.Args) < 4 {
		return nil
	}
	keyValue := g.info.Types[call.Args[1]].Value
	variants, ok := call.Args[3].(*ast.CompositeLit)
	if keyValue == nil || keyValue.Kind() != constant.String || !ok {
		return nil
	}
	u := &unionInfo{Key: constant.StringVal(keyValue)}
	for _, elt := range variants.Elts {
		lit, isLit := elt.(*ast.CompositeLit)
		if !isLit {
			return nil
		}
		var c unionCase
		for _, e := range lit.Elts {
			kv, isKV := e.(*ast.KeyValueExpr)
			key, isIdent := kv.Key.(*ast.Ident)
			if !isKV || !isIdent {
				return nil
			}
			switch key.Name {
			case "Value":
				v := g.info.Types[kv.Value].Value
				if v == nil {
					return nil
				}
				switch v.Kind() {
				case constant.String:
					c.Value = strconv.Quote(constant.StringVal(v))
				case constant.Int:
					c.Value = v.ExactString()
				default:
					return nil
				}
			case "StructType":
				// reflect.TypeFor[T]()
				tc, isCall := kv.Value.(*ast.CallExpr)
				if !isCall {
					return nil
				}
				idx, isIndex := tc.Fun.(*ast.IndexExpr)
				if !isIndex {
					return nil
				}
				c.typ = g.info.Types[idx.Index].Type
			}
		}
		if c.Value == "" {
			return nil
		}
		u.Cases = append(u.Cases, c)
	}
	return u
}

// ensureUnion generates a union once: its shared fields (the discriminator
// among them) as usual, plus a member for each variant struct - the json:"-"
// field of that type, as Go keeps it.
func (g *generator) ensureUnion(named *types.Named, u *unionInfo) string {
	name := named.Obj().Name()
	if _, done := g.structs[name]; done || g.busy[name] {
		return name
	}
	g.busy[name] = true
	st := named.Underlying().(*types.Struct)
	fields := g.flattenFields(st, map[string]bool{})
	for _, f := range fields {
		if f.JSONName == u.Key {
			u.Field = f.CppName
		}
	}
	if u.Field == "" {
		log.Fatalf("%s: discriminator %q is not one of its fields", name, u.Key)
	}
	for i := range u.Cases {
		c := &u.Cases[i]
		if c.typ == nil {
			continue
		}
		for j := 0; j < st.NumFields(); j++ {
			v := st.Field(j)
			jsonTag, _ := reflect.StructTag(st.Tag(j)).Lookup("json")
			variant, isNamed := v.Type().(*types.Named)
			if jsonTag != "-" || !isNamed || !types.Identical(v.Type(), c.typ) {
				continue
			}
			c.Member = goToCppFieldName(v.Name())
			fields = append(fields, field{CppName: c.Member, CppType: g.ensure(variant), Variant: true})
			break
		}
		if c.Member == "" {
			log.Fatalf("%s: no json:\"-\" field holds variant %s", name, c.typ)
		}
	}
	delete(g.busy, name)
	g.structs[name] = fields
	g.unions[name] = u
	g.order = append(g.order, name)
	return name
}

// ensure generates named (a struct of g.pkg) once and returns its C++ name.
func (g *generator) ensure(named *types.Named) string {
	name := named.Obj().Name()
	if _, done := g.structs[name]; done || g.busy[name] {
		return name
	}
	g.busy[name] = true
	fields := g.flattenFields(named.Underlying().(*types.Struct), map[string]bool{})
	delete(g.busy, name)
	g.structs[name] = fields
	g.order = append(g.order, name)
	return name
}

// hasCustomJSONMethods reports whether t (or *t) implements any of the
// method names Go's encoding/json treats specially. If so we must not try
// to structurally flatten or auto-map it — its JSON shape is whatever that
// method produces, which we can't infer from field types alone.
func hasCustomJSONMethods(t types.Type) bool {
	names := []string{"MarshalJSON", "UnmarshalJSON", "MarshalJSONContext", "UnmarshalJSONContext"}
	check := func(typ types.Type) bool {
		ms := types.NewMethodSet(typ)
		for _, n := range names {
			if sel := ms.Lookup(nil, n); sel != nil {
				return true
			}
		}
		return false
	}
	if check(t) {
		return true
	}
	return check(types.NewPointer(t))
}

// hasTextMethods: encoding.TextMarshaler/TextUnmarshaler - JSON then holds
// whatever text the method makes, not the underlying value.
func hasTextMethods(t types.Type) bool {
	for _, typ := range []types.Type{t, types.NewPointer(t)} {
		ms := types.NewMethodSet(typ)
		if ms.Lookup(nil, "MarshalText") != nil || ms.Lookup(nil, "UnmarshalText") != nil {
			return true
		}
	}
	return false
}

func (g *generator) flattenFields(st *types.Struct, seen map[string]bool) []field {
	pkg := g.pkg
	var out []field
	for i := 0; i < st.NumFields(); i++ {
		v := st.Field(i)
		tag := reflect.StructTag(st.Tag(i))
		jsonTag, hasJSON := tag.Lookup("json")
		jsonName, omitEmpty := parseJSONTag(jsonTag, v.Name())
		if hasJSON && jsonName == "-" {
			continue
		}

		if v.Anonymous() {
			underlying := v.Type()
			ptr := false
			if p, isPtr := underlying.(*types.Pointer); isPtr {
				underlying = p.Elem()
				ptr = true
			}
			named, isNamed := underlying.(*types.Named)
			if isNamed && !ptr && !hasCustomJSONMethods(underlying) {
				if es, isStruct := underlying.Underlying().(*types.Struct); isStruct {
					if named.Obj().Pkg() != nil && named.Obj().Pkg().Path() == pkg.Path() {
						key := named.Obj().Name()
						if !seen[key] {
							seen[key] = true
							out = append(out, g.flattenFields(es, seen)...)
							continue
						}
					}
				}
			}
			// Foreign package, pointer, or custom-marshaled embed: can't
			// safely flatten. Fall through to opaque handling below using
			// the Go field/type name as the JSON key guess — a human must
			// verify this against the real wire format when writing the
			// adapter.
			out = append(out, opaqueField(v, jsonName, omitEmpty, "anonymous field with non-trivial JSON semantics: "+describeType(v.Type())))
			continue
		}

		cppType, needAdapter, note := g.mapType(v.Type())
		out = append(out, field{
			CppName:     goToCppFieldName(v.Name()),
			CppType:     cppType,
			JSONName:    jsonName,
			OmitEmpty:   omitEmpty,
			NeedAdapter: needAdapter,
			AdapterNote: note,
			IntType:     integerOf(cppType),
		})
	}
	return out
}

func opaqueField(v *types.Var, jsonName string, omitEmpty bool, note string) field {
	name := jsonName
	if name == "" {
		name = strings.ToLower(v.Name())
	}
	return field{
		CppName:     goToCppFieldName(v.Name()),
		CppType:     "nlohmann::json",
		JSONName:    name,
		OmitEmpty:   omitEmpty,
		NeedAdapter: true,
		AdapterNote: note,
	}
}

func parseJSONTag(tag, goName string) (name string, omitEmpty bool) {
	if tag == "" {
		return goName, false
	}
	parts := strings.Split(tag, ",")
	name = parts[0]
	if name == "" {
		name = goName
	}
	for _, p := range parts[1:] {
		if p == "omitempty" {
			omitEmpty = true
		}
	}
	return name, omitEmpty
}

// mapType maps a Go field type to a C++ type. Returns needAdapter=true for
// anything whose JSON (de)serialization we can't safely auto-generate yet
// (badoption.* custom types, unmapped composites) — those become raw
// nlohmann::json members for a handwritten adapter to replace later.
func (g *generator) mapType(t types.Type) (cppType string, needAdapter bool, note string) {
	if p, ok := t.(*types.Pointer); ok {
		inner, adapt, n := g.mapType(p.Elem())
		return "std::optional<" + inner + ">", adapt, n
	}
	// []byte: encoding/json writes it as one base64 string. Kept as that text -
	// it round-trips exactly, and nothing on our side reads the bytes yet (the
	// one user today is tls.certificate_public_key_sha256, pinning).
	if sl, ok := t.(*types.Slice); ok {
		if b, isBasic := sl.Elem().(*types.Basic); isBasic && b.Kind() == types.Uint8 {
			return "std::string", false, "[]byte, kept as its base64 JSON text"
		}
		// Any other slice: a JSON array (nil and empty alike are "empty" to omitempty).
		elemCpp, elemAdapt, elemNote := g.mapType(sl.Elem())
		if !elemAdapt {
			return "std::vector<" + elemCpp + ">", false, elemNote
		}
		return "nlohmann::json", true, "[]" + sl.Elem().String() + ": " + elemNote
	}
	// A map with string keys: a JSON object (badoption.HTTPHeader is one).
	if m, ok := t.Underlying().(*types.Map); ok && !hasCustomJSONMethods(t) {
		if k, isBasic := m.Key().Underlying().(*types.Basic); isBasic && k.Kind() == types.String {
			elemCpp, elemAdapt, elemNote := g.mapType(m.Elem())
			if !elemAdapt {
				return "std::map<std::string, " + elemCpp + ">", false, elemNote
			}
		}
	}
	if basic, ok := t.(*types.Basic); ok {
		switch basic.Kind() {
		case types.String:
			return "std::string", false, ""
		case types.Bool:
			return "bool", false, ""
		case types.Int, types.Int64:
			return "std::int64_t", false, ""
		case types.Int8:
			return "std::int8_t", false, ""
		case types.Int16:
			return "std::int16_t", false, ""
		case types.Int32:
			return "std::int32_t", false, ""
		case types.Uint, types.Uint64:
			return "std::uint64_t", false, ""
		case types.Uint8:
			return "std::uint8_t", false, ""
		case types.Uint16:
			return "std::uint16_t", false, ""
		case types.Uint32:
			return "std::uint32_t", false, ""
		case types.Float32:
			return "float", false, ""
		case types.Float64:
			return "double", false, ""
		}
	}
	if named, ok := t.(*types.Named); ok {
		pkgPath := ""
		if named.Obj().Pkg() != nil {
			pkgPath = named.Obj().Pkg().Path()
		}
		if pkgPath == "github.com/sagernet/sing/common/json/badoption" {
			switch named.Obj().Name() {
			case "Duration":
				return "sovereign::adapters::Duration", false, ""
			case "Addr":
				// badoption.Addr marshals as a plain IP-address string; we
				// store it as one rather than adding a structured IP type,
				// since nothing needs to inspect/construct these
				// programmatically yet (deferred to whenever dialer code
				// does).
				return "std::string", false, ""
			}
		}
		if pkgPath == "github.com/sagernet/sing-box/option" && named.Obj().Name() == "FwMark" {
			return "sovereign::adapters::FwMark", false, ""
		}
		if pkgPath == "github.com/sagernet/sing-box/option" && named.Obj().Name() == "CurvePreference" {
			return "sovereign::adapters::CurvePreference", false, ""
		}
		if pkgPath == "github.com/sagernet/sing/common/json/badoption" && named.Obj().Name() == "Listable" {
			if targs := named.TypeArgs(); targs != nil && targs.Len() == 1 {
				elemCpp, elemAdapt, elemNote := g.mapType(targs.At(0))
				if !elemAdapt {
					return "sovereign::adapters::Listable<" + elemCpp + ">", false, elemNote
				}
				return "nlohmann::json", true, "Listable[" + targs.At(0).String() + "]: " + elemNote
			}
		}
		// net/netip's addresses and prefixes marshal as their text form.
		if pkgPath == "net/netip" {
			switch named.Obj().Name() {
			case "Addr", "Prefix", "AddrPort":
				return "std::string", false, "netip." + named.Obj().Name() + " as its text"
			}
		}
		// A plain struct of the package being generated: generated too.
		if g.pkg != nil && pkgPath == g.pkg.Path() && !hasCustomJSONMethods(named) {
			if _, isStruct := named.Underlying().(*types.Struct); isStruct {
				return g.ensure(named), false, ""
			}
		}
		// A union sing-box describes in its schema (DescribeSchema ->
		// schema.DiscriminatedUnion): the shared fields plus the one variant
		// the discriminator picks, merged into one object - generated as such.
		if g.pkg != nil && pkgPath == g.pkg.Path() {
			if _, isStruct := named.Underlying().(*types.Struct); isStruct {
				if u := g.findUnion(named); u != nil {
					return g.ensureUnion(named, u), false, ""
				}
			}
		}
		// A named string/number/bool without methods of its own: what it's made of.
		if _, isBasic := named.Underlying().(*types.Basic); isBasic && !hasCustomJSONMethods(named) && !hasTextMethods(named) {
			return g.mapType(named.Underlying())
		}
		return "nlohmann::json", true, "unmapped named type " + pkgPath + "." + named.Obj().Name() + " — needs a handwritten adapter"
	}
	return "nlohmann::json", true, "unmapped Go type " + t.String() + " — needs a handwritten adapter"
}

func describeType(t types.Type) string {
	return t.String()
}

// goToCppFieldName converts a Go exported field name to camelCase,
// treating a leading run of acronym capitals as one unit (TCPFastOpen ->
// tcpFastOpen, NetNs -> netNs) instead of just lowercasing the first rune.
func goToCppFieldName(goName string) string {
	runes := []rune(goName)
	if len(runes) == 0 {
		return goName
	}
	i := 0
	for i < len(runes) && unicode.IsUpper(runes[i]) {
		i++
	}
	switch {
	case i == 0:
		return goName
	case i == len(runes):
		return strings.ToLower(goName)
	case i == 1:
		return strings.ToLower(string(runes[0])) + string(runes[1:])
	case !unicode.IsLetter(runes[i]):
		// The acronym ends at a digit, not at the next word: DNS01Challenge -> dns01Challenge.
		return strings.ToLower(string(runes[:i])) + string(runes[i:])
	default:
		return strings.ToLower(string(runes[:i-1])) + string(runes[i-1:])
	}
}

func usesType(fields []field, needle string) bool {
	for _, f := range fields {
		if strings.Contains(f.CppType, needle) {
			return true
		}
	}
	return false
}

func usesOmitEmpty(fields []field) bool {
	for _, f := range fields {
		if f.OmitEmpty {
			return true
		}
	}
	return false
}

func countAdapters(fields []field) int {
	n := 0
	for _, f := range fields {
		if f.NeedAdapter {
			n++
		}
	}
	return n
}

func (g *generator) renderHeader(namespace, typeName string, sourcePkg string) string {
	var all []field
	for _, name := range g.order {
		all = append(all, g.structs[name]...)
	}

	var b strings.Builder
	fmt.Fprintf(&b, "// Code generated by tools/codegen from %s (%s). DO NOT EDIT.\n", sourcePkg, typeName)
	fmt.Fprintf(&b, "// Fields marked \"needs adapter\" are raw nlohmann::json passthroughs pending\n")
	fmt.Fprintf(&b, "// a handwritten adapter (see Sovereign issue #2) — they round-trip losslessly\n")
	fmt.Fprintf(&b, "// but are not yet typed.\n")
	b.WriteString("#pragma once\n\n")
	b.WriteString("#include <cstdint>\n")
	if usesType(all, "std::map<") {
		b.WriteString("#include <map>\n")
	}
	b.WriteString("#include <optional>\n")
	b.WriteString("#include <string>\n")
	if usesType(all, "std::vector<") {
		b.WriteString("#include <vector>\n")
	}
	b.WriteString("\n")
	b.WriteString("#include <nlohmann/json.hpp>\n")
	if usesType(all, "sovereign::adapters::Listable<") {
		b.WriteString("#include <adapters/listable.h>\n")
	}
	if usesType(all, "sovereign::adapters::Duration") {
		b.WriteString("#include <adapters/duration.h>\n")
	}
	if usesType(all, "sovereign::adapters::FwMark") {
		b.WriteString("#include <adapters/fwmark.h>\n")
	}
	if usesType(all, "sovereign::adapters::CurvePreference") {
		b.WriteString("#include <adapters/curve_preference.h>\n")
	}
	for _, f := range all {
		if f.IntType != "" {
			b.WriteString("#include <adapters/integer.h>\n")
			break
		}
	}
	if usesOmitEmpty(all) {
		b.WriteString("#include <adapters/omit_empty.h>\n")
	}
	b.WriteString("\n")

	parts := strings.Split(namespace, "::")
	for _, p := range parts {
		fmt.Fprintf(&b, "namespace %s {\n", p)
	}

	// Dependencies first: g.order is the order ensure() finished them in.
	for _, name := range g.order {
		b.WriteString("\n")
		g.renderStruct(&b, name)
	}
	b.WriteString("\n")

	for range parts {
		b.WriteString("}\n")
	}
	return b.String()
}

func (g *generator) renderStruct(b *strings.Builder, typeName string) {
	fields := g.structs[typeName]
	union := g.unions[typeName]
	fmt.Fprintf(b, "struct %s {\n", typeName)
	names := make([]string, len(fields))
	for i, f := range fields {
		line := fmt.Sprintf("  %s %s{};", f.CppType, f.CppName)
		if f.AdapterNote != "" {
			line += "  // " + f.AdapterNote
		}
		b.WriteString(line + "\n")
		names[i] = f.CppName
	}
	sort.Strings(names) // just to catch accidental dup field names below
	for i := 1; i < len(names); i++ {
		if names[i] == names[i-1] {
			fmt.Fprintf(os.Stderr, "warning: duplicate C++ field name %q in %s after flattening — check embedded struct name collisions\n", names[i], typeName)
		}
	}
	b.WriteString("};\n\n")

	// A struct with no fields of its own (V2RayQUICOptions): nothing reads v or j.
	empty := true
	for _, f := range fields {
		empty = empty && f.Variant
	}
	vName, jName := "v", "j"
	if empty && union == nil {
		vName, jName = "/*v*/", "/*j*/"
	}
	fmt.Fprintf(b, "inline void to_json(nlohmann::json& j, const %s& %s) {\n", typeName, vName)
	b.WriteString("  j = nlohmann::json::object();\n")
	for _, f := range fields {
		if f.Variant {
			continue
		}
		// omitempty never drops a struct in Go (isEmptyValue has no struct case).
		if _, isStruct := g.structs[f.CppType]; f.OmitEmpty && !isStruct {
			fmt.Fprintf(b, "  if (!sovereign::adapters::IsEmptyValue(v.%s)) { j[%q] = v.%s; }\n",
				f.CppName, f.JSONName, f.CppName)
		} else {
			fmt.Fprintf(b, "  j[%q] = v.%s;\n", f.JSONName, f.CppName)
		}
	}
	// A union: the variant the discriminator picks, merged in (badjson.MarshallObjects).
	// Go refuses an unknown one; here it's just the shared fields.
	if union != nil {
		for _, c := range union.Cases {
			if c.Member != "" {
				fmt.Fprintf(b, "  if (v.%s == %s) { j.update(nlohmann::json(v.%s)); }\n", union.Field, c.Value, c.Member)
			}
		}
	}
	b.WriteString("}\n\n")

	// Every field is optional on the way in, omitempty or not: Go's unmarshal
	// never requires a key - a missing one leaves the zero value (omitempty only
	// shapes the output). Reading non-omitempty fields with at() made a config
	// without, say, server_port unparsable here while sing-box takes it.
	fmt.Fprintf(b, "inline void from_json(const nlohmann::json& %s, %s& %s) {\n", jName, typeName, vName)
	for _, f := range fields {
		if f.Variant {
			continue
		}
		read := fmt.Sprintf("v.%s = j.at(%q).get<decltype(v.%s)>();", f.CppName, f.JSONName, f.CppName)
		if f.IntType != "" {
			read = fmt.Sprintf("v.%s = sovereign::adapters::GetInteger<%s>(j.at(%q));", f.CppName, f.IntType, f.JSONName)
		}
		fmt.Fprintf(b, "  if (j.contains(%q)) { %s }\n", f.JSONName, read)
	}
	// A union's variant comes from the same object (badjson.UnmarshallExcluded).
	if union != nil {
		for _, c := range union.Cases {
			if c.Member != "" {
				fmt.Fprintf(b, "  if (v.%s == %s) { from_json(j, v.%s); }\n", union.Field, c.Value, c.Member)
			}
		}
	}
	b.WriteString("}\n")
}
