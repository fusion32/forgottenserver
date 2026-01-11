package main

import (
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"math/big"
	"os"
)

func main() {
	// NOTE(fusion): Generate public key from known N and E.
	const (
		N = "BC27F992A96B8E2A43F4DFBE1CEF8FD51CF43D2803EE34FBBD8634D8B4FA32F7" +
			"D9D9E159978DD29156D62F4153E9C5914263FC4986797E12245C1A6C4531EFE4" +
			"8A6F7C2EFFFFF18F2C9E1C504031F3E4A2C788EE96618FFFCEC2C3E5BFAFAF74" +
			"3B3FC7A872EE60A52C29AA688BDAF8692305312882F1F66EE9D8AEB7F84B1949"

		E = "10001"
	)

	n, nok := new(big.Int).SetString(N, 16)
	e, eok := new(big.Int).SetString(E, 16)
	if !nok || !eok || !e.IsInt64() || !e.ProbablyPrime(4) {
		panic("something is not quite right")
	}

	publicKey := rsa.PublicKey{N: n, E: int(e.Int64())}
	block := pem.Block{
		Type:  "RSA PUBLIC KEY",
		Bytes: x509.MarshalPKCS1PublicKey(&publicKey),
	}
	pem.Encode(os.Stdout, &block)
}
