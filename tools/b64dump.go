package main

import (
	"encoding/base64"
	"fmt"
	"io"
	"log"
	"os"
	"strings"
)

func PrintBuffer(name string, buf []byte) {
	const bytesPerLine = 16
	builder := strings.Builder{}
	numLines := (len(buf) + bytesPerLine - 1) / bytesPerLine
	log.Printf("%v (size=%v, lines=%v):", name, len(buf), numLines)
	for i := 0; i < numLines; i += 1 {
		offset := i * bytesPerLine
		count := min(bytesPerLine, len(buf)-offset)
		line := buf[offset:][:count]

		fmt.Fprintf(&builder, "%16v | ", offset)
		for j := 0; j < bytesPerLine; j += 1 {
			if j > 0 {
				builder.WriteByte(' ')
			}

			if j < count {
				fmt.Fprintf(&builder, "%02X", line[j])
			} else {
				builder.WriteString("  ")
			}
		}

		builder.WriteString(" | ")

		for j := 0; j < count; j += 1 {
			if line[j] >= 32 && line[j] <= 126 { // printable ascii
				builder.WriteByte(line[j])
			} else {
				builder.WriteByte('.')
			}
		}

		log.Print(builder.String())
		builder.Reset()
	}
}

func main() {
	var (
		err   error
		input []byte
	)

	if len(os.Args) > 1 {
		input, err = os.ReadFile(os.Args[1])
	} else {
		input, err = io.ReadAll(os.Stdin)
	}

	if err != nil {
		fmt.Printf("failed to read input: %v\n", err)
		os.Exit(1)
	}

	output := make([]byte, base64.StdEncoding.DecodedLen(len(input)))
	_, err = base64.StdEncoding.Decode(output, input)
	if err != nil {
		fmt.Printf("failed to decode input: %v\n", err)
		os.Exit(1)
	}

	PrintBuffer("OUTPUT", output)
}
