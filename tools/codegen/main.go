// Command codegen reads a struct from a pinned sing-box option package via
// go/packages (real type-checking, not regex-over-source) and emits a C++
// header with a matching struct plus nlohmann::json to_json/from_json.
//
// Scope for this first slice (see Sovereign issue #2): only flatten
// anonymous embedded fields that are plain data (no custom
// MarshalJSON/UnmarshalJSON) and live in the same package. Anything that
// needs real Go JSON semantics we can't safely infer here — badoption.*
// types, foreign embedded types, custom (un)marshalers — is emitted as a
// raw nlohmann::json member with a comment, left for the handwritten
// adapter layer (P1's remaining checklist items) to replace.
package main

import (
	"flag"
	"fmt"
	"go/types"
	"log"
	"os"
	"reflect"
	"sort"
	"strings"
	"unicode"

	"golang.org/x/tools/go/packages"
)

type field struct {
	CppName    string
	CppType    string
	JSONName   string
	OmitEmpty  bool
	NeedAdapter bool
	AdapterNote string
}

func main() {
	src := flag.String("src", "", "path to the sing-box repo root (contains go.mod)")
	pkgPath := flag.String("pkg", "./option", "package pattern to load, relative to -src")
	typeName := flag.String("type", "", "struct name to generate, e.g. AnyTLSOutboundOptions")
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

	obj := pkg.Types.Scope().Lookup(*typeName)
	if obj == nil {
		log.Fatalf("type %s not found in package %s", *typeName, pkg.PkgPath)
	}
	named, ok := obj.Type().(*types.Named)
	if !ok {
		log.Fatalf("%s is not a named type", *typeName)
	}
	structType, ok := named.Underlying().(*types.Struct)
	if !ok {
		log.Fatalf("%s is not a struct", *typeName)
	}

	fields := flattenFields(pkg.Types, structType, map[string]bool{})

	src2 := renderHeader(*namespace, *typeName, fields, pkg.PkgPath)

	if *out == "-" {
		fmt.Print(src2)
		return
	}
	if err := os.WriteFile(*out, []byte(src2), 0o644); err != nil {
		log.Fatalf("write %s: %v", *out, err)
	}
	fmt.Fprintf(os.Stderr, "wrote %s (%d fields, %d need adapters)\n", *out, len(fields), countAdapters(fields))
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

func flattenFields(pkg *types.Package, st *types.Struct, seen map[string]bool) []field {
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
							out = append(out, flattenFields(pkg, es, seen)...)
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

		cppType, needAdapter, note := mapType(v.Type())
		out = append(out, field{
			CppName:     goToCppFieldName(v.Name()),
			CppType:     cppType,
			JSONName:    jsonName,
			OmitEmpty:   omitEmpty,
			NeedAdapter: needAdapter,
			AdapterNote: note,
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
func mapType(t types.Type) (cppType string, needAdapter bool, note string) {
	if p, ok := t.(*types.Pointer); ok {
		inner, adapt, n := mapType(p.Elem())
		return "std::optional<" + inner + ">", adapt, n
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
		if pkgPath == "github.com/sagernet/sing/common/json/badoption" && named.Obj().Name() == "Listable" {
			if targs := named.TypeArgs(); targs != nil && targs.Len() == 1 {
				elemCpp, elemAdapt, elemNote := mapType(targs.At(0))
				if !elemAdapt {
					return "sovereign::adapters::Listable<" + elemCpp + ">", false, ""
				}
				return "nlohmann::json", true, "Listable[" + targs.At(0).String() + "]: " + elemNote
			}
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

func renderHeader(namespace, typeName string, fields []field, sourcePkg string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "// Code generated by tools/codegen from %s (%s). DO NOT EDIT.\n", sourcePkg, typeName)
	fmt.Fprintf(&b, "// Fields marked \"needs adapter\" are raw nlohmann::json passthroughs pending\n")
	fmt.Fprintf(&b, "// a handwritten adapter (see Sovereign issue #2) — they round-trip losslessly\n")
	fmt.Fprintf(&b, "// but are not yet typed.\n")
	b.WriteString("#pragma once\n\n")
	b.WriteString("#include <cstdint>\n")
	b.WriteString("#include <optional>\n")
	b.WriteString("#include <string>\n\n")
	b.WriteString("#include <nlohmann/json.hpp>\n")
	if usesType(fields, "sovereign::adapters::Listable<") {
		b.WriteString("#include <adapters/listable.h>\n")
	}
	if usesType(fields, "sovereign::adapters::Duration") {
		b.WriteString("#include <adapters/duration.h>\n")
	}
	if usesType(fields, "sovereign::adapters::FwMark") {
		b.WriteString("#include <adapters/fwmark.h>\n")
	}
	if usesOmitEmpty(fields) {
		b.WriteString("#include <adapters/omit_empty.h>\n")
	}
	b.WriteString("\n")

	parts := strings.Split(namespace, "::")
	for _, p := range parts {
		fmt.Fprintf(&b, "namespace %s {\n", p)
	}
	b.WriteString("\n")

	fmt.Fprintf(&b, "struct %s {\n", typeName)
	names := make([]string, len(fields))
	for i, f := range fields {
		line := fmt.Sprintf("  %s %s{};", f.CppType, f.CppName)
		if f.NeedAdapter {
			line += "  // " + f.AdapterNote
		}
		b.WriteString(line + "\n")
		names[i] = f.CppName
	}
	sort.Strings(names) // just to catch accidental dup field names below
	for i := 1; i < len(names); i++ {
		if names[i] == names[i-1] {
			fmt.Fprintf(os.Stderr, "warning: duplicate C++ field name %q after flattening — check embedded struct name collisions\n", names[i])
		}
	}
	b.WriteString("};\n\n")

	fmt.Fprintf(&b, "inline void to_json(nlohmann::json& j, const %s& v) {\n", typeName)
	b.WriteString("  j = nlohmann::json::object();\n")
	for _, f := range fields {
		if f.OmitEmpty {
			fmt.Fprintf(&b, "  if (!sovereign::adapters::IsEmptyValue(v.%s)) { j[%q] = v.%s; }\n",
				f.CppName, f.JSONName, f.CppName)
		} else {
			fmt.Fprintf(&b, "  j[%q] = v.%s;\n", f.JSONName, f.CppName)
		}
	}
	b.WriteString("}\n\n")

	fmt.Fprintf(&b, "inline void from_json(const nlohmann::json& j, %s& v) {\n", typeName)
	for _, f := range fields {
		if f.OmitEmpty {
			fmt.Fprintf(&b, "  if (j.contains(%q)) { v.%s = j.at(%q).get<decltype(v.%s)>(); }\n",
				f.JSONName, f.CppName, f.JSONName, f.CppName)
		} else {
			fmt.Fprintf(&b, "  v.%s = j.at(%q).get<decltype(v.%s)>();\n", f.CppName, f.JSONName, f.CppName)
		}
	}
	b.WriteString("}\n\n")

	for range parts {
		b.WriteString("}\n")
	}
	return b.String()
}
