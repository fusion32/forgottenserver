package main

// NOTE(fusion): This is basically a standalone version of Gesior's IP-Changer.
//	https://github.com/gesior/ots-ip-changer-12/blob/master/exeEditor.php
//	https://otland.net/threads/disable-battleye-error-12-20.266831/

import (
	"bytes"
	"crypto/rsa"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"math/big"
	"os"
	"path"
	"path/filepath"
	"reflect"
	"strings"
)

type (
	Config struct {
		URLs struct {
			TibiaPageUrl                 string `json:"tibiaPageUrl"`
			TibiaStoreGetCoinsUrl        string `json:"tibiaStoreGetCoinsUrl"`
			GetPremiumUrl                string `json:"getPremiumUrl"`
			CreateAccountUrl             string `json:"createAccountUrl"`
			CreateTournamentCharacterUrl string `json:"createTournamentCharacterUrl"`
			AccessAccountUrl             string `json:"accessAccountUrl"`
			LostAccountUrl               string `json:"lostAccountUrl"`
			ManualUrl                    string `json:"manualUrl"`
			FaqUrl                       string `json:"faqUrl"`
			PremiumFeaturesUrl           string `json:"premiumFeaturesUrl"`
			LimesurveyUrl                string `json:"limesurveyUrl"`
			HintsUrl                     string `json:"hintsUrl"`
			CipSoftUrl                   string `json:"cipSoftUrl"`
			TwitchTibiaUrl               string `json:"twitchTibiaUrl"`
			YouTubeTibiaUrl              string `json:"youTubeTibiaUrl"`
			CrashReportUrl               string `json:"crashReportUrl"`
			FpsHistoryRecipient          string `json:"fpsHistoryRecipient"`
			TutorialProgressWebService   string `json:"tutorialProgressWebService"`
			TournamentDetailsUrl         string `json:"tournamentDetailsUrl"`
			LoginWebService              string `json:"loginWebService"`
			ClientWebService             string `json:"clientWebService"`
		}

		RSAModulus string
	}

	Patch struct {
		Description string
		Original    []byte
		Replacement []byte
	}

	Client struct {
		Version string
		Patches []Patch
	}
)

var (
	Clients = []Client{
		{
			Version: "12.20",
			Patches: []Patch{
				{
					Description: "BattlEye 12.20",
					Original:    []byte{0x84, 0xC0, 0x74, 0x04, 0xC6, 0x47, 0x05, 0x01},
					Replacement: []byte{0x84, 0xC0, 0x90, 0x90, 0xC6, 0x47, 0x05, 0x01},
				},
			},
		},
		{
			Version: "12.40",
			Patches: []Patch{
				{
					Description: "BattlEye 12.40.9997",
					Original:    []byte{0xC6, 0x45, 0xD7, 0x00, 0xC6, 0x45, 0xCF, 0x00},
					Replacement: []byte{0xC6, 0x45, 0xD7, 0x00, 0xC6, 0x45, 0xCF, 0x01},
				},
			},
		},
		{
			Version: "12.72",
			Patches: []Patch{
				{
					Description: "BattlEye 12.72.11272",
					Original:    []byte{0x8D, 0x8D, 0x70, 0xFF, 0xFF, 0xFF, 0x75, 0x0E},
					Replacement: []byte{0x8D, 0x8D, 0x70, 0xFF, 0xFF, 0xFF, 0xEB, 0x0E},
				},
			},
		},
		{
			Version: "12.81",
			Patches: []Patch{
				{
					Description: "BattlEye 12.81.11476",
					Original:    []byte{0x8D, 0x8D, 0x70, 0xFF, 0xFF, 0xFF, 0x75, 0x0E},
					Replacement: []byte{0x8D, 0x8D, 0x70, 0xFF, 0xFF, 0xFF, 0xEB, 0x0E},
				},
			},
		},
		{
			Version: "12.85",
			Patches: []Patch{
				{
					Description: "BattlEye 12.85",
					Original:    []byte{0x8D, 0x8D, 0x70, 0xFF, 0xFF, 0xFF, 0x75, 0x0E},
					Replacement: []byte{0x8D, 0x8D, 0x70, 0xFF, 0xFF, 0xFF, 0xEB, 0x0E},
				},
			},
		},
		{
			Version: "12.86",
			Patches: []Patch{
				{
					Description: "BattlEye 12.86",
					Original:    []byte{0x8D, 0x4D, 0x84, 0x75, 0x0E, 0xE8, 0x1B, 0x41},
					Replacement: []byte{0x8D, 0x4D, 0x84, 0xEB, 0x0E, 0xE8, 0x1B, 0x41},
				},
			},
		},
		{
			Version: "12.87",
			Patches: []Patch{
				{
					Description: "BattlEye 12.87",
					Original:    []byte{0x8D, 0x4D, 0x84, 0x75, 0x0E, 0xE8, 0xF8, 0x24},
					Replacement: []byte{0x8D, 0x4D, 0x84, 0xEB, 0x0E, 0xE8, 0xF8, 0x24},
				},
			},
		},
		{
			Version: "12.90",
			Patches: []Patch{
				{
					Description: "BattlEye 12.90",
					Original:    []byte{0x8D, 0x4D, 0x8C, 0x75},
					Replacement: []byte{0x8D, 0x4D, 0x8C, 0xEB},
				},
			},
		},
		{
			Version: "12.91",
			Patches: []Patch{
				{
					Description: "BattlEye 12.91.12329",
					Original:    []byte{0x00, 0x00, 0x00, 0x8D, 0x4D, 0x80, 0x75, 0x0E, 0xE8},
					Replacement: []byte{0x00, 0x00, 0x00, 0x8D, 0x4D, 0x80, 0xEB, 0x0E, 0xE8},
				},
			},
		},
		{
			Version: "13.05",
			Patches: []Patch{
				{
					Description: "BattlEye 13.05.12715",
					Original:    []byte{0x8D, 0x4D, 0xB4, 0x75},
					Replacement: []byte{0x8D, 0x4D, 0xB4, 0xEB},
				},
			},
		},
		{
			Version: "13.10",
			Patches: []Patch{
				{
					Description: "BattlEye 13.10.12858",
					Original:    []byte{0x8D, 0x4D, 0xB8, 0x75},
					Replacement: []byte{0x8D, 0x4D, 0xB8, 0xEB},
				},
				{
					Description: "BattlEye 13.10.12892",
					Original:    []byte{0x8D, 0x4D, 0xB8, 0x75, 0x0E, 0xE8, 0xEA, 0x42, 0xF3},
					Replacement: []byte{0x8D, 0x4D, 0xB8, 0xEB, 0x0E, 0xE8, 0xEA, 0x42, 0xF3},
				},
				{
					Description: "BattlEye 13.11.12985",
					Original:    []byte{0x8D, 0x4D, 0xC0, 0x51, 0x3B, 0x45, 0xE4, 0x74, 0x0D, 0x8B, 0xC8},
					Replacement: []byte{0x8D, 0x4D, 0xC0, 0x51, 0x3B, 0x45, 0xE4, 0xEB, 0x0D, 0x8B, 0xC8},
				},
			},
		},
		{
			Version: "13.20",
			Patches: []Patch{
				{
					Description: "BattlEye 13.20",
					Original:    []byte{0x75, 0x0E, 0xE8, 0xB5},
					Replacement: []byte{0xEB, 0x0E, 0xE8, 0xB5},
				},
				{
					Description: "BattlEye 13.20.13709",
					Original:    []byte{0xFF, 0xFF, 0xFF, 0x75, 0x0E, 0xE8, 0xDF},
					Replacement: []byte{0xFF, 0xFF, 0xFF, 0xEB, 0x0E, 0xE8, 0xDF},
				},
			},
		},
		{
			Version: "13.21",
			Patches: []Patch{
				{
					Description: "BattlEye 13.21.13810",
					Original:    []byte{0xFF, 0xFF, 0xFF, 0x75, 0x0E, 0xE8, 0xCF},
					Replacement: []byte{0xFF, 0xFF, 0xFF, 0xEB, 0x0E, 0xE8, 0xCF},
				},
			},
		},
		{
			Version: "13.22",
			Patches: []Patch{
				{
					Description: "BattlEye 13.22.14242",
					Original:    []byte{0x8D, 0x4D, 0xB4, 0x75, 0x0E, 0xE8},
					Replacement: []byte{0x8D, 0x4D, 0xB4, 0xEB, 0x0E, 0xE8},
				},
			},
		},
		{
			Version: "13.34",
			Patches: []Patch{
				{
					Description: "BattlEye 13.34.14631",
					Original:    []byte{0x8D, 0x4D, 0xB4, 0x75, 0x0E, 0xE8},
					Replacement: []byte{0x8D, 0x4D, 0xB4, 0xEB, 0x0E, 0xE8},
				},
			},
		},
		{
			Version: "13.40",
			Patches: []Patch{
				{
					Description: "BattlEye 13.40.54ea79",
					Original:    []byte{0x8D, 0x4D, 0xB4, 0x75, 0x0E, 0xE8},
					Replacement: []byte{0x8D, 0x4D, 0xB4, 0xEB, 0x0E, 0xE8},
				},
			},
		},
		{
			Version: "14.11",
			Patches: []Patch{
				{
					Description: "BattlEye 14.11.0fbf6c",
					Original:    []byte{0x00, 0x00, 0x00, 0x75, 0x0F, 0xE8, 0xC3, 0x43, 0xEF, 0xFF},
					Replacement: []byte{0x00, 0x00, 0x00, 0xEB, 0x0F, 0xE8, 0xC3, 0x43, 0xEF, 0xFF},
				},
			},
		},
		{
			Version: "15.03",
			Patches: []Patch{
				{
					Description: "BattlEye 15.03.afe753",
					Original:    []byte{0x75, 0x0F, 0xE8, 0xDF},
					Replacement: []byte{0xEB, 0x0F, 0xE8, 0xDF},
				},
			},
		},
		{
			Version: "15.11",
			Patches: []Patch{
				{
					Description: "BattlEye 15.11.57e218",
					Original:    []byte{0x75, 0x0F, 0xE8, 0xC1, 0xEE, 0xFF},
					Replacement: []byte{0xEB, 0x0F, 0xE8, 0xC1, 0xEE, 0xFF},
				},
			},
		},
		{
			Version: "15.20",
			Patches: []Patch{
				{
					Description: "BattlEye 15.20.8007a5",
					Original:    []byte{0x75, 0x0F, 0xE8, 0x73, 0x20, 0xEE, 0xFF},
					Replacement: []byte{0xEB, 0x0F, 0xE8, 0x73, 0x20, 0xEE, 0xFF},
				},
			},
		},
	}
)

func ApplyPatches(data []byte, patches []Patch) {
	for _, p := range patches {
		offset := bytes.Index(data, p.Original)
		if offset == -1 {
			continue
		}

		fmt.Printf("Applying patch %v @%X...\n", p.Description, offset)
		if bytes.Index(data[offset+len(p.Original):], p.Original) != -1 {
			fmt.Printf("ERROR: Patch %v original sequence is NOT unique\n",
				p.Description)
		} else if len(p.Original) != len(p.Replacement) {
			fmt.Printf("ERROR: Patch %v original and replacement sequences"+
				" have different sizes (%v and %v respectively)\n",
				p.Description, len(p.Original), len(p.Replacement))
		} else {
			copy(data[offset:], p.Replacement)
		}
	}
}

func ReadConfig(configPath string) (config Config, err error) {
	var data []byte
	if configPath == "" {
		data, err = io.ReadAll(os.Stdin)
	} else {
		data, err = os.ReadFile(configPath)
	}

	if err == nil {
		err = json.Unmarshal(data, &config)
	}
	return
}

func ApplyConfig(data []byte, config Config, rsaOutputPath string) {
	fmt.Printf("Applying config...\n")

	{ // NOTE(fusion): Update URLs.
		typ := reflect.TypeOf(config.URLs)
		val := reflect.ValueOf(config.URLs)
		for i := 0; i < typ.NumField(); i += 1 {
			fieldVal := val.Field(i)
			if fieldVal.Kind() != reflect.String {
				continue
			}

			fieldTyp := typ.Field(i)
			key, _, _ := strings.Cut(fieldTyp.Tag.Get("json"), ",")
			if key == "" {
				key = fieldTyp.Name
			} else if key == "-" {
				continue
			}

			newURL := fieldVal.String()
			if newURL == "" {
				continue
			}

			if !strings.HasPrefix(newURL, "http://") && !strings.HasPrefix(newURL, "https://") {
				fmt.Printf("ERROR: Expected HTTP(S) protocol for URL: %v=%v", key, newURL)
				continue
			}

			offset := bytes.Index(data, []byte(key+"="))
			if offset == -1 {
				continue
			}

			start := offset + len(key) + 1
			count := bytes.Index(data[start:], []byte{'\n'})
			if count != -1 {
				if count > 0 && data[start+count-1] == '\r' {
					count -= 1
				}
			} else {
				count = len(data) - start
			}

			oldURL := string(data[start : start+count])
			if len(newURL) > len(oldURL) {
				fmt.Printf("ERROR: URL for %v is too large (len=%v, max=%v)\n",
					key, len(newURL), len(oldURL))
				continue
			}

			fmt.Printf("%v @%X:\n", key, offset)
			fmt.Printf(" - %v\n", oldURL)
			fmt.Printf(" + %v\n", newURL)

			copy(data[start:], newURL)
			for i := len(newURL); i < len(oldURL); i += 1 {
				data[start+i] = ' '
			}
		}
	}

	// NOTE(fusion): Update RSA modulus.
	if config.RSAModulus != "" {
		start := 0
		count := 0
		rsaCount := len(config.RSAModulus)
		for i, b := range data {
			if b >= '0' && b <= '9' ||
				b >= 'A' && b <= 'F' ||
				b >= 'a' && b <= 'f' {
				count += 1
			} else {
				if count >= rsaCount {
					break
				}
				start = i + 1
				count = 0
			}
		}

		if count >= rsaCount {
			oldRsaModulus := string(data[start : start+count])
			if rsaOutputPath != "" {
				fmt.Printf("RSAModulus @%X -> %v:\n", start, rsaOutputPath)
				if err := SaveRsaPublicKey(rsaOutputPath, oldRsaModulus); err != nil {
					fmt.Printf("ERROR: Unable to save RSA modulus: %v\n", err)
				}
			} else {
				fmt.Printf("RSAModulus @%X:\n", start)
			}

			fmt.Printf(" - %v\n", oldRsaModulus)
			fmt.Printf(" + %v\n", config.RSAModulus)
			copy(data[start:], config.RSAModulus)
			for i := rsaCount; i < count; i += 1 {
				data[start+i] = 0
			}
		} else {
			fmt.Printf("ERROR: Unable to find RSA modulus (count=%v).", rsaCount)
		}
	}
}

func SaveRsaPublicKey(name string, modulus string) (err error) {
	n, ok := new(big.Int).SetString(modulus, 16)
	if !ok {
		err = errors.New("unable to parse RSA modulus")
		return
	}

	publicKey := rsa.PublicKey{N: n, E: 0x10001}
	block := pem.Block{
		Type:  "RSA PUBLIC KEY",
		Bytes: x509.MarshalPKCS1PublicKey(&publicKey),
	}

	var fp *os.File
	fp, err = os.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return
	}

	defer fp.Close()
	return pem.Encode(fp, &block)
}

func WriteFile(name string, data []byte, perm fs.FileMode) (err error) {
	// NOTE(fusion): Directories need execute permission to work properly
	// so we add it for the groups that have read permission.
	executePerm := (perm & 0444) >> 2

	dir := path.Dir(name)
	err = os.MkdirAll(dir, perm|executePerm)
	if err != nil {
		return
	}

	var fp *os.File
	fp, err = os.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, perm)
	if err != nil {
		return
	}

	defer fp.Close()
	_, err = fp.Write(data)
	return
}

func main() {
	var (
		outputPath    string
		configPath    string
		rsaOutputPath string
	)

	flag.StringVar(&outputPath, "out", "", "")
	flag.StringVar(&configPath, "cfg", "", "")
	flag.StringVar(&rsaOutputPath, "rsa", "", "")
	flag.Parse()

	if flag.NArg() != 1 {
		fmt.Printf("USAGE: patcher.exe [options...] CLIENT.EXE\n")
		flag.PrintDefaults()
		os.Exit(1)
	}

	config, err := ReadConfig(configPath)
	if err != nil {
		fmt.Printf("failed to read config \"%v\": %v\n", configPath, err)
		os.Exit(1)
	}

	inputPath := filepath.ToSlash(flag.Arg(0))
	if outputPath == "" {
		outputPath = inputPath + ".out"
	}

	data, err := os.ReadFile(inputPath)
	if err != nil {
		fmt.Printf("failed to read file \"%v\": %v\n", inputPath, err)
		os.Exit(1)
	}

	for _, c := range Clients {
		if bytes.Index(data, []byte(c.Version)) != -1 {
			fmt.Printf("Patching client %v...\n", c.Version)
			ApplyPatches(data, c.Patches)
			break
		}
	}

	ApplyConfig(data, config, rsaOutputPath)

	fmt.Printf("writing to \"%v\"...\n", outputPath)
	if err := WriteFile(outputPath, data, 0755); err != nil {
		fmt.Printf("Failed to write file \"%v\": %v\n", outputPath, err)
		os.Exit(1)
	}

	os.Exit(0)
}
