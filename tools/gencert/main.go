// Command gencert writes an ephemeral self-signed TLS certificate and key
// to disk, PEM-encoded. It exists to stand up local TLS-based sing-box
// test fixtures (anytls today, any other TLS-wrapped protocol later)
// without committing private key material to the repo — dev/test tooling,
// not part of Sovereign's own TLS code path.
//
// With -reality it instead prints a REALITY X25519 key pair in the exact
// format `sing-box generate reality-keypair` uses (base64 RawURL,
// `PrivateKey:`/`PublicKey:` lines), for local REALITY fixtures.
package main

import (
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/pem"
	"flag"
	"fmt"
	"math/big"
	"net"
	"os"
	"time"
)

func main() {
	host := flag.String("host", "127.0.0.1", "IP or DNS name the certificate is valid for")
	certPath := flag.String("out-cert", "cert.pem", "output path for the PEM certificate")
	keyPath := flag.String("out-key", "key.pem", "output path for the PEM private key")
	reality := flag.Bool("reality", false, "print a REALITY X25519 key pair instead of writing a certificate")
	flag.Parse()

	if *reality {
		if err := printRealityKeyPair(); err != nil {
			fmt.Fprintln(os.Stderr, "gencert:", err)
			os.Exit(1)
		}
		return
	}

	if err := run(*host, *certPath, *keyPath); err != nil {
		fmt.Fprintln(os.Stderr, "gencert:", err)
		os.Exit(1)
	}
}

func run(host, certPath, keyPath string) error {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return fmt.Errorf("generate key: %w", err)
	}

	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return fmt.Errorf("generate serial: %w", err)
	}

	template := &x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: host},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	if ip := net.ParseIP(host); ip != nil {
		template.IPAddresses = []net.IP{ip}
	} else {
		template.DNSNames = []string{host}
	}

	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return fmt.Errorf("create certificate: %w", err)
	}

	if err := writePEM(certPath, "CERTIFICATE", der); err != nil {
		return err
	}

	keyBytes, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return fmt.Errorf("marshal key: %w", err)
	}
	return writePEM(keyPath, "EC PRIVATE KEY", keyBytes)
}

func writePEM(path, blockType string, bytes []byte) error {
	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create %s: %w", path, err)
	}
	defer f.Close()
	return pem.Encode(f, &pem.Block{Type: blockType, Bytes: bytes})
}

// X25519 clamps the scalar when it is used, so the raw random bytes are a
// valid REALITY private key as-is — same as sing-box's wgtypes-based command.
func printRealityKeyPair() error {
	key, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return fmt.Errorf("generate x25519 key: %w", err)
	}
	fmt.Println("PrivateKey: " + base64.RawURLEncoding.EncodeToString(key.Bytes()))
	fmt.Println("PublicKey: " + base64.RawURLEncoding.EncodeToString(key.PublicKey().Bytes()))
	return nil
}
