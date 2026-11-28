module github.com/sovereignbrains/sovereign/tools/goldengen

go 1.27.0

replace github.com/sagernet/sing-box => ../../vendor/sing-box

require (
	github.com/sagernet/sing v0.9.7-0.20261127131305-db09250ecaed
	github.com/sagernet/sing-box v0.0.0-00010101000000-000000000000
)

require (
	github.com/miekg/dns v1.1.72 // indirect
	go4.org/netipx v0.0.0-20231129151722-fdeea329fbba // indirect
	golang.org/x/mod v0.38.0 // indirect
	golang.org/x/net v0.58.0 // indirect
	golang.org/x/sync v0.22.0 // indirect
	golang.org/x/sys v0.49.0 // indirect
	golang.org/x/tools v0.48.0 // indirect
)
