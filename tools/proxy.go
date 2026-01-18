package main

import (
	"bytes"
	"compress/gzip"
	"compress/zlib"
	"crypto/rsa"
	"crypto/x509"
	"encoding/binary"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

/*
// ZLIB Interop
// ==============================================================================
// IMPORTANT(fusion): The client now uses a single inflate stream with no FINAL
// blocks in between. Each packet is compressed with Z_SYNC_FLUSH and stripped
// of the "\x00\x00\xFF\xFF" sequence at the end.
//  Go's "flate" package does handle raw deflate but doesn't seem to support
// streaming data, which is what we need here. For that reason, I'm using CGo to
// interop with ZLIB, which I honestly didn't think would work very well, but does.

//#cgo CFLAGS: -g
#cgo LDFLAGS: -lz

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

z_stream *ZStreamAlloc(void){
	return (z_stream*)malloc(sizeof(z_stream));
}

void ZStreamFree(z_stream *strm){
	free(strm);
}

int InflateInitWrapper(z_stream *strm){
	return inflateInit2(strm, -15);
}
*/
import "C"

func InflateDestroy(strm *C.z_stream) {
	C.inflateEnd(strm)
	C.ZStreamFree(strm)
}

func InflateNew() (strm *C.z_stream, err error) {
	strm = C.ZStreamAlloc()
	ret := C.InflateInitWrapper(strm)
	if ret != C.Z_OK {
		err = fmt.Errorf("failed to initialize inflate stream: (%v) %v",
			ret, InflateMessage(strm))
	}
	return
}

func InflateMessage(strm *C.z_stream) string {
	msg := "no message"
	if strm.msg != nil {
		msg = unsafe.String((*byte)(unsafe.Pointer(strm.msg)), C.strlen(strm.msg))
	}
	return msg
}

func InflateSyncFlush(strm *C.z_stream, input []byte) (output []byte, err error) {
	buffer := bytes.Buffer{}
	strm.next_in = (*C.Bytef)(unsafe.SliceData(input))
	strm.avail_in = (C.uInt)(len(input))
	for strm.avail_in > 0 {
		var tmp [4096]byte
		strm.next_out = (*C.Bytef)(unsafe.Pointer(&tmp))
		strm.avail_out = (C.uInt)(len(tmp))
		ret := C.inflate(strm, C.Z_SYNC_FLUSH)
		if ret != C.Z_OK {
			err = fmt.Errorf("failed to inflate stream: (%v) %v",
				ret, InflateMessage(strm))
			return
		}

		n := (len(tmp) - int(strm.avail_out))
		buffer.Write(tmp[:n])
	}

	output = buffer.Bytes()
	return
}

// JSON Types
// ==============================================================================
type (
	LoginRequest struct {
		Type          string `json:"type"`
		AssetVersion  string `json:"assetversion"`
		ClientType    int    `json:"clienttype"`
		ClientVersion string `json:"clientversion"`
		DeviceCookie  string `json:"devicecookie"`
		Email         string `json:"email"`
		Password      string `json:"password"`
		StayLoggedIn  bool   `json:"stayloggedin"`
	}

	Session struct {
		SessionKey            string `json:"sessionkey"`
		LastLoginTime         int    `json:"lastlogintime"`
		IsPremium             bool   `json:"ispremium"`
		PremiumUntil          int    `json:"premiumuntil"`
		Status                string `json:"status"`
		ReturnerNotification  bool   `json:"returnernotification"`
		ShowRewardNews        bool   `json:"showrewardnews"`
		IsReturner            bool   `json:"isreturner"`
		RecoverySetupComplete bool   `json:"recoverysetupcomplete"`
		FpsTracking           bool   `json:"fpstracking"`
		OptionTracking        bool   `json:"optiontracking"`
	}

	World struct {
		Id                         int    `json:"id"`
		Name                       string `json:"name"`
		ExternalAddressProtected   string `json:"externaladdressprotected"`
		ExternalPortProtected      int    `json:"externalportprotected"`
		ExternalAddressUnprotected string `json:"externaladdressunprotected"`
		ExternalPortUnprotected    int    `json:"externalportunprotected"`
		PreviewState               int    `json:"previewstate"`
		Location                   string `json:"location"`
		AntiCheatProtection        bool   `json:"anticheatprotection"`
		PvpType                    int    `json:"pvptype"`
	}

	Character struct {
		WorldId          int    `json:"worldid"`
		Name             string `json:"name"`
		Level            int    `json:"level"`
		Vocation         string `json:"vocation"`
		IsMale           bool   `json:"ismale"`
		IsHidden         bool   `json:"ishidden"`
		IsMainCharacter  bool   `json:"ismaincharacter"`
		Tutorial         bool   `json:"tutorial"`
		OutfitId         int    `json:"outfitid"`
		HeadColor        int    `json:"headcolor"`
		TorsoColor       int    `json:"torsocolor"`
		LegsColor        int    `json:"legscolor"`
		DetailColor      int    `json:"detailcolor"`
		AddonsFlags      int    `json:"addonsflags"`
		DailyRewardState int    `json:"dailyrewardstate"`
	}

	PlayData struct {
		Worlds     []World     `json:"worlds"`
		Characters []Character `json:"characters"`
	}

	LoginResponse struct {
		Session      Session  `json:"session"`
		PlayData     PlayData `json:"playdata"`
		DeviceCookie string   `json:"devicecookie"`
		LoginEmail   string   `json:"loginemail"`
	}
)

// World Addresses
// ==============================================================================
var (
	g_WorldMapMutex sync.Mutex
	g_WorldMap      = map[string]string{}
)

func SetWorldAddress(worldName, worldAddress string) {
	g_WorldMapMutex.Lock()
	defer g_WorldMapMutex.Unlock()
	g_WorldMap[worldName] = worldAddress
}

func GetWorldAddress(worldName string) string {
	g_WorldMapMutex.Lock()
	defer g_WorldMapMutex.Unlock()
	return g_WorldMap[worldName]
}

// RSA
// ==============================================================================
var (
	g_RsaPrivateKey *rsa.PrivateKey
	g_RsaPublicKey  *rsa.PublicKey
)

func RsaLoadPublicKey(path string) (*rsa.PublicKey, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	block, _ := pem.Decode(data)
	if block == nil {
		return nil, errors.New("no key found")
	}

	return x509.ParsePKCS1PublicKey(block.Bytes)
}

func RsaLoadPrivateKey(path string) (*rsa.PrivateKey, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	block, _ := pem.Decode(data)
	if block == nil {
		return nil, errors.New("no key found")
	}

	return x509.ParsePKCS1PrivateKey(block.Bytes)
}

func RsaEncryptNoPadding(key *rsa.PublicKey, data []byte) {
	// NOTE(fusion): The NO_PADDING mode for RSA encryption/decryption is probably
	// one of the few utilities missing from Go's stdlib so we have to do it manually.
	// Note that we're not using the CRT or any kind of optimizations for simplicity,
	// since we also don't need to be the most performant here.
	if len(data) != key.Size() {
		log.Panicf("key size mismatch (data=%v, key=%v)", len(data), key.Size())
	}
	plaintext := new(big.Int).SetBytes(data)
	E := new(big.Int).SetInt64(int64(key.E))
	ciphertext := new(big.Int).Exp(plaintext, E, key.N)
	ciphertext.FillBytes(data)
}

func RsaDecryptNoPadding(key *rsa.PrivateKey, data []byte) {
	// NOTE(fusion): Same as `RsaEncryptNoPadding`.
	if len(data) != key.Size() {
		log.Panicf("key size mismatch (data=%v, key=%v)", len(data), key.Size())
	}
	ciphertext := new(big.Int).SetBytes(data)
	plaintext := new(big.Int).Exp(ciphertext, key.D, key.N)
	plaintext.FillBytes(data)
}

// XTEA
// ==============================================================================
func XTEAEncrypt(key [4]uint32, data []byte) {
	if len(data)%8 != 0 {
		log.Panic("data size needs to be multiple of 8")
	}

	const delta = uint32(0x9E3779B9)
	for i := 0; i < len(data); i += 8 {
		v0 := binary.LittleEndian.Uint32(data[i:])
		v1 := binary.LittleEndian.Uint32(data[i+4:])
		sum := uint32(0)
		for j := 0; j < 32; j += 1 {
			v0 += ((v1<<4 ^ v1>>5) + v1) ^ (sum + key[sum&3])
			sum += delta
			v1 += ((v0<<4 ^ v0>>5) + v0) ^ (sum + key[sum>>11&3])
		}
		binary.LittleEndian.PutUint32(data[i:], v0)
		binary.LittleEndian.PutUint32(data[i+4:], v1)
	}
}

func XTEADecrypt(key [4]uint32, data []byte) {
	if len(data)%8 != 0 {
		log.Panic("data size needs to be multiple of 8")
	}

	const delta = uint32(0x9E3779B9)
	for i := 0; i < len(data); i += 8 {
		v0 := binary.LittleEndian.Uint32(data[i:])
		v1 := binary.LittleEndian.Uint32(data[i+4:])
		sum := uint32(0xC6EF3720)
		for j := 0; j < 32; j += 1 {
			v1 -= ((v0<<4 ^ v0>>5) + v0) ^ (sum + key[sum>>11&3])
			sum -= delta
			v0 -= ((v1<<4 ^ v1>>5) + v1) ^ (sum + key[sum&3])
		}
		binary.LittleEndian.PutUint32(data[i:], v0)
		binary.LittleEndian.PutUint32(data[i+4:], v1)
	}
}

// Buffer Reader
// ==============================================================================
type BufReader struct {
	Buffer   []byte
	Position int
}

func (r *BufReader) CanRead(n int) bool {
	return r.Position+n <= len(r.Buffer)
}

func (r *BufReader) Overflowed() bool {
	return r.Position > len(r.Buffer)
}

func (r *BufReader) BytesAvailable() int {
	return max(0, len(r.Buffer)-r.Position)
}

func (r *BufReader) Remainder() []byte {
	result := []byte{}
	if !r.Overflowed() {
		result = r.Buffer[r.Position:]
	}
	return result
}

func (r *BufReader) DiscardPadding(n int) bool {
	if n > r.BytesAvailable() {
		return false
	}
	r.Buffer = r.Buffer[:len(r.Buffer)-n]
	return true
}

func (r *BufReader) ReadFlag() bool {
	return r.Read8() != 0
}

func (r *BufReader) Read8() uint8 {
	result := uint8(0)
	if r.CanRead(1) {
		result = r.Buffer[r.Position]
	}
	r.Position += 1
	return result
}

func (r *BufReader) Read16() uint16 {
	result := uint16(0)
	if r.CanRead(2) {
		result = binary.LittleEndian.Uint16(r.Buffer[r.Position:])
	}
	r.Position += 2
	return result
}

func (r *BufReader) Read16BE() uint16 {
	result := uint16(0)
	if r.CanRead(2) {
		result = binary.BigEndian.Uint16(r.Buffer[r.Position:])
	}
	r.Position += 2
	return result
}

func (r *BufReader) Read32() uint32 {
	result := uint32(0)
	if r.CanRead(4) {
		result = binary.LittleEndian.Uint32(r.Buffer[r.Position:])
	}
	r.Position += 4
	return result
}

func (r *BufReader) Read32BE() uint32 {
	result := uint32(0)
	if r.CanRead(4) {
		result = binary.BigEndian.Uint32(r.Buffer[r.Position:])
	}
	r.Position += 4
	return result
}

func (r *BufReader) Read64() uint64 {
	result := uint64(0)
	if r.CanRead(8) {
		result = binary.LittleEndian.Uint64(r.Buffer[r.Position:])
	}
	r.Position += 8
	return result
}

func (r *BufReader) Read64BE() uint64 {
	result := uint64(0)
	if r.CanRead(8) {
		result = binary.BigEndian.Uint64(r.Buffer[r.Position:])
	}
	r.Position += 8
	return result
}

func (r *BufReader) ReadBytes(n int) []byte {
	result := []byte{}
	if r.CanRead(n) {
		result = r.Buffer[r.Position:][:n]
	}
	r.Position += n
	return result
}

func (r *BufReader) ReadString() string {
	// TODO(fusion): Does the latest client still uses LATIN1?
	stringLen := int(r.Read16())
	return string(r.ReadBytes(stringLen))
}

// Buffer Writer
// ==============================================================================
type BufWriter struct {
	Buffer   []byte
	Position int
}

func (w *BufWriter) CanWrite(n int) bool {
	return w.Position+n <= len(w.Buffer)
}

func (w *BufWriter) Overflowed() bool {
	return w.Position > len(w.Buffer)
}

func (w *BufWriter) CanRewrite(position int, n int) bool {
	return position+n <= w.Position && !w.Overflowed()
}

func (w *BufWriter) WriteFlag(value bool) {
	if value {
		w.Write8(0x01)
	} else {
		w.Write8(0x00)
	}
}

func (w *BufWriter) Write8(value uint8) {
	if w.CanWrite(1) {
		w.Buffer[w.Position] = value
	}
	w.Position += 1
}

func (w *BufWriter) Write16(value uint16) {
	if w.CanWrite(2) {
		binary.LittleEndian.PutUint16(w.Buffer[w.Position:], value)
	}
	w.Position += 2
}

func (w *BufWriter) Write16BE(value uint16) {
	if w.CanWrite(2) {
		binary.BigEndian.PutUint16(w.Buffer[w.Position:], value)
	}
	w.Position += 2
}

func (w *BufWriter) Write32(value uint32) {
	if w.CanWrite(4) {
		binary.LittleEndian.PutUint32(w.Buffer[w.Position:], value)
	}
	w.Position += 4
}

func (w *BufWriter) Write32BE(value uint32) {
	if w.CanWrite(4) {
		binary.BigEndian.PutUint32(w.Buffer[w.Position:], value)
	}
	w.Position += 4
}

func (w *BufWriter) Write64(value uint64) {
	if w.CanWrite(8) {
		binary.LittleEndian.PutUint64(w.Buffer[w.Position:], value)
	}
	w.Position += 8
}

func (w *BufWriter) Write64BE(value uint64) {
	if w.CanWrite(8) {
		binary.BigEndian.PutUint64(w.Buffer[w.Position:], value)
	}
	w.Position += 8
}

func (w *BufWriter) WriteBytes(data []byte) {
	if w.CanWrite(len(data)) {
		copy(w.Buffer[w.Position:], data)
	}
	w.Position += len(data)
}

func (w *BufWriter) WriteString(s string) {
	// TODO(fusion): Does the latest client still uses LATIN1?
	n := min(0xFFFF, len(s))
	if n != len(s) {
		log.Printf("string truncated (%v -> %v)", len(s), n)
	}
	w.Write16(uint16(n))
	w.WriteBytes([]byte(s[:n]))
}

func (w *BufWriter) Rewrite8(position int, value uint8) {
	if w.CanRewrite(position, 1) {
		w.Buffer[position] = value
	}
}

func (w *BufWriter) Rewrite16(position int, value uint16) {
	if w.CanRewrite(position, 2) {
		binary.LittleEndian.PutUint16(w.Buffer[position:], value)
	}
}

func (w *BufWriter) Rewrite32(position int, value uint32) {
	if w.CanRewrite(position, 4) {
		binary.LittleEndian.PutUint32(w.Buffer[position:], value)
	}
}

// HTTP Proxy
// ==============================================================================
var (
	g_RequestCounter atomic.Int32
	g_ServiceMap     = map[string]string{
		"/loginservice.php":   "https://www.tibia.com/clientservices/loginservice.php",
		"/clientservices.php": "https://www.tibia.com/clientservices/clientservices.php",
	}
)

func CompressGZIP(input []byte) (output []byte, err error) {
	var buf bytes.Buffer
	w := gzip.NewWriter(&buf)
	if _, err = w.Write(input); err != nil {
		return
	}
	if err = w.Close(); err != nil {
		return
	}
	output, err = io.ReadAll(&buf)
	return
}

func DecompressGZIP(input []byte) (output []byte, err error) {
	var r io.ReadCloser
	if r, err = gzip.NewReader(bytes.NewReader(input)); err != nil {
		return
	}
	if output, err = io.ReadAll(r); err != nil {
		return
	}
	err = r.Close()
	return
}

func CompressZLIB(input []byte) (output []byte, err error) {
	var buf bytes.Buffer
	w := zlib.NewWriter(&buf)
	if _, err = w.Write(input); err != nil {
		return
	}
	if err = w.Close(); err != nil {
		return
	}
	output, err = io.ReadAll(&buf)
	return
}

func DecompressZLIB(input []byte) (output []byte, err error) {
	var r io.ReadCloser
	if r, err = zlib.NewReader(bytes.NewReader(input)); err != nil {
		return
	}
	if output, err = io.ReadAll(r); err != nil {
		return
	}
	err = r.Close()
	return
}

func RelayClientRequest(req *http.Request) (reqBody []byte, res *http.Response, err error) {
	url, ok := g_ServiceMap[req.URL.Path]
	if !ok {
		err = fmt.Errorf("no service mapping for \"%v\"", req.URL.Path)
		return
	}

	reqBody, err = io.ReadAll(req.Body)
	if err != nil {
		return
	}

	var newReq *http.Request
	newReq, err = http.NewRequest(req.Method, url, bytes.NewReader(reqBody))
	if err != nil {
		return
	}

	newReq.Header = req.Header.Clone()
	res, err = http.DefaultClient.Do(newReq)
	return
}

func DecodeContent(input []byte, encoding string) (output []byte, err error) {
	switch encoding {
	case "", "identity": // no-op
		output = input
	case "gzip":
		output, err = DecompressGZIP(input)
	case "deflate":
		output, err = DecompressZLIB(input)
	default:
		// TODO(fusion): Might want to restrict the "Accept-Encoding"
		// header that is relayed to the actual server?
		err = fmt.Errorf("unsupported encoding (%v)", encoding)
	}
	return
}

func EncodeContent(input []byte, encoding string) (output []byte, err error) {
	switch encoding {
	case "", "identity": // no-op
		output = input
	case "gzip":
		output, err = CompressGZIP(input)
	case "deflate":
		output, err = CompressZLIB(input)
	default:
		// TODO(fusion): Same as `DecodeContent`.
		err = fmt.Errorf("unsupported encoding (%v)", encoding)
	}
	return
}

func IsLoginRequest(input []byte) bool {
	var tmp struct {
		Type string `json:"type"`
	}

	err := json.Unmarshal(input, &tmp)
	if err != nil {
		log.Printf("failed to parse request data: %v", err)
		return false
	}

	return tmp.Type == "login"
}

func SaveAndRewriteWorldEndpoints(input []byte) (output []byte, err error) {
	var res LoginResponse
	if err = json.Unmarshal(input, &res); err != nil {
		return
	}

	for i := range res.PlayData.Worlds {
		world := &res.PlayData.Worlds[i]

		// NOTE(fusion): Save world address.
		worldAddr := net.JoinHostPort(world.ExternalAddressProtected,
			strconv.Itoa(world.ExternalPortProtected))
		SetWorldAddress(world.Name, worldAddr)

		// NOTE(fusion): Reroute world to our own local server.
		world.ExternalAddressProtected = "localhost"
		world.ExternalPortProtected = g_GamePort
		world.ExternalAddressUnprotected = "localhost"
		world.ExternalPortUnprotected = g_GamePort
	}

	output, err = json.Marshal(res)
	return
}

func HttpRequestHandler(w http.ResponseWriter, req *http.Request) {
	var (
		err    error
		res    *http.Response
		input  []byte
		output []byte
	)

	input, res, err = RelayClientRequest(req)
	if err != nil {
		log.Printf("failed to relay client's request: %v", err)
		return
	}

	output, err = io.ReadAll(res.Body)
	if err != nil {
		log.Printf("failed to read response body: %v", err)
		return
	}

	{
		var decodedInput, decodedOutput []byte

		decodedInput, err = DecodeContent(input, req.Header.Get("Content-Encoding"))
		if err != nil {
			log.Printf("failed to decode request content: %v", err)
			return
		}

		decodedOutput, err = DecodeContent(output, res.Header.Get("Content-Encoding"))
		if err != nil {
			log.Printf("failed to decode response content: %v", err)
			return
		}

		requestId := g_RequestCounter.Add(1)
		log.Printf("HTTP REQUEST:  (%v) [[%v]]", requestId, string(decodedInput))
		log.Printf("HTTP RESPONSE: (%v) [[%v]]", requestId, string(decodedOutput))

		if IsLoginRequest(decodedInput) {
			decodedOutput, err = SaveAndRewriteWorldEndpoints(decodedOutput)
			if err != nil {
				log.Printf("failed to save and rewrite world endpoints: %v", err)
				return
			}

			output, err = EncodeContent(decodedOutput, res.Header.Get("Content-Encoding"))
			if err != nil {
				log.Printf("failed to encode response content: %v", err)
				return
			}
		}
	}

	// NOTE(fusion): Forward headers and data back to the client.
	for k, values := range res.Header {
		for _, v := range values {
			w.Header().Add(k, v)
		}
	}

	// NOTE(fusion): Always override the content length to make sure it has the
	// correct value, in case we modified the response.
	w.Header().Set("Content-Length", strconv.Itoa(len(output)))

	w.WriteHeader(res.StatusCode)
	w.Write(output)
}

func RunHttpProxy() {
	httpAddr := net.JoinHostPort("localhost", strconv.Itoa(g_HttpPort))
	log.Printf("HTTP server listening on %v...", httpAddr)
	log.Print(http.ListenAndServe(httpAddr, http.HandlerFunc(HttpRequestHandler)))
}

// Game Proxy
// ==============================================================================
type PlainGamePacket struct {
	sequence   uint32
	compressed bool
	padding    int
	payload    []byte
}

func ReadByte(r io.Reader) (byte, error) {
	var buf [1]byte
	_, err := io.ReadFull(r, buf[:])
	return buf[0], err
}

func ReadLine(r io.Reader) (string, error) {
	// TODO(fusion): This is definitely wasteful, but we're only using it for
	// reading the world name at the beginning of a new client connection. The
	// alternative would be to use a larger stack buffer and rely on the fact
	// that this initial packet only contains a single line of text (probably?)
	// so we would not overread.
	var (
		err error
		b   byte
		buf []byte
	)
	for {
		b, err = ReadByte(r)
		if err != nil {
			break
		}
		buf = append(buf, b)
		if b == '\n' {
			break
		}
	}
	return string(buf), err
}

func ReadGamePacketLen(src []byte) int {
	numXteaBlocks := int(binary.LittleEndian.Uint16(src))
	packetLen := 4 + 8*numXteaBlocks
	return packetLen
}

func WriteGamePacketLen(dst []byte, packetLen int) error {
	const minPacketLen = 4 + 8
	const maxPacketLen = 4 + 8*0xFFFF

	if packetLen < minPacketLen {
		return errors.New("packet length is too short")
	} else if packetLen > maxPacketLen {
		return errors.New("packet length is too long")
	} else if packetLen%8 != 4 {
		return errors.New("packet length is invalid")
	}

	numXteaBlocks := (packetLen - 4) / 8
	binary.LittleEndian.PutUint16(dst, uint16(numXteaBlocks))
	return nil
}

func ReadGamePacket(r io.Reader) (data []byte, err error) {
	var (
		packetLenBuffer [2]byte
		n               int
	)

	n, err = io.ReadFull(r, packetLenBuffer[:])
	if err != nil {
		return
	}

	packetLen := ReadGamePacketLen(packetLenBuffer[:])
	data = make([]byte, packetLen)
	n, err = io.ReadFull(r, data)
	if err != nil {
		data = data[:n]
	}
	return
}

func WriteGamePacket(w io.Writer, data []byte) (n int, err error) {
	var packetLenBuffer [2]byte
	err = WriteGamePacketLen(packetLenBuffer[:], len(data))
	if err != nil {
		return
	}

	_, err = w.Write(packetLenBuffer[:])
	if err != nil {
		return
	}

	return w.Write(data)
}

func ForwardNextPacket(w io.Writer, r io.Reader) (data []byte, err error) {
	data, err = ReadGamePacket(r)
	if err != nil {
		return
	}

	_, err = WriteGamePacket(w, data)
	return
}

func LogBuffer(name string, buf []byte) {
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

func InterceptGameHandshake(client net.Conn, server net.Conn) (xteaKey [4]uint32, err error) {
	var (
		challenge []byte
		login     []byte
	)

	// NOTE(fusion): Forward server challenge directly.
	challenge, err = ForwardNextPacket(client, server)
	if err != nil {
		err = fmt.Errorf("failed to forward server challenge: %w", err)
		return
	}

	LogBuffer("server -> client [Challenge]", challenge)

	// NOTE(fusion): Read login packet, decrypt with our private key, extract
	// the XTEA key, encrypt with the public key, and forward it.
	login, err = ReadGamePacket(client)
	if err != nil {
		err = fmt.Errorf("failed to read login packet: %w", err)
		return
	}

	{
		// IMPORTANT(fusion): This is version specific. It will yield an error
		// if the packet isn't properly parsed, so it's be a good idea to run
		// without intercepting first, to get a grasp of the packet format.
		//      - Current version: 15.20.6bd207
		const loginPacketLen = 260
		if len(login) != loginPacketLen {
			err = fmt.Errorf("invalid login packet length (expected %v, got %v)", loginPacketLen, len(login))
			return
		}

		r := BufReader{Buffer: login, Position: 0}
		r.Read32()                         // sequence
		padding := int(r.Read8())          // padding
		r.Read8()                          // 0x0A => GAME_LOGIN ?
		r.Read16()                         // terminal type
		r.Read16()                         // terminal version
		r.Read32()                         // terminal version 32?
		r.ReadString()                     // version string
		r.ReadString()                     // hex string => client/assets checksum ?
		r.Read8()                          // ??
		asymmetricData := r.ReadBytes(128) // asymmetric data
		r.ReadBytes(padding)               // padding bytes

		if r.Overflowed() {
			err = fmt.Errorf("login packet not properly parsed")
			return
		}

		if r.BytesAvailable() != 0 {
			err = fmt.Errorf("login packet not fully parsed")
			return
		}

		RsaDecryptNoPadding(g_RsaPrivateKey, asymmetricData)
		if asymmetricData[0] != 0 {
			err = fmt.Errorf("login packet has malformed asymmetric data")
			return
		}

		LogBuffer("server <- client [Login]", login)
		xteaKey[0] = binary.LittleEndian.Uint32(asymmetricData[1:])
		xteaKey[1] = binary.LittleEndian.Uint32(asymmetricData[5:])
		xteaKey[2] = binary.LittleEndian.Uint32(asymmetricData[9:])
		xteaKey[3] = binary.LittleEndian.Uint32(asymmetricData[13:])

		RsaEncryptNoPadding(g_RsaPublicKey, asymmetricData)
	}

	_, err = WriteGamePacket(server, login)
	if err != nil {
		err = fmt.Errorf("failed to write login packet: %w", err)
		return
	}

	return
}

func InterceptGamePacket(xteaKey [4]uint32, inflateStream *C.z_stream, input []byte) (packet PlainGamePacket, err error) {
	if len(input)%8 != 4 {
		err = fmt.Errorf("invalid packet length (size=%v)", len(input))
		return
	}

	sequence := binary.LittleEndian.Uint32(input[0:])
	compressed := (sequence&0xC0000000 == 0xC0000000)
	XTEADecrypt(xteaKey, input[4:])
	padding := int(input[4])
	payload := input[5:]
	if padding >= 8 || padding >= len(payload) {
		err = fmt.Errorf("invalid padding (%v)", padding)
		return
	}

	payload = payload[:len(payload)-padding]
	if compressed {
		// NOTE(fusion): This "\x00\x00\xFF\xFF" sequence is part of the output
		// of deflate with mode Z_SYNC_FLUSH which gets stripped before being
		// added to a packet. There is a small comment about it in the link below,
		// which is referenced by zlib's home page.
		//  HREF: https://www.bolet.org/~pornin/deflate-flush.html
		payload = append(payload, 0x00, 0x00, 0xFF, 0xFF)
		if inflateStream != nil {
			payload, err = InflateSyncFlush(inflateStream, payload)
			if err != nil {
				return
			}
		}
	}

	packet = PlainGamePacket{
		sequence:   sequence,
		compressed: compressed,
		padding:    padding,
		payload:    payload,
	}
	return
}

func GameConnectionHandler(client net.Conn) {
	defer client.Close()
	log.Printf("new connection from %v", client.RemoteAddr())

	worldLine, err := ReadLine(client)
	if err != nil {
		log.Printf("%v: failed to retrieve world name: %v", client.RemoteAddr(), err)
		return
	}

	worldName := strings.TrimSpace(worldLine)
	if g_WorldOverride != "" {
		worldName = g_WorldOverride
	}

	worldAddr := GetWorldAddress(worldName)
	if worldAddr == "" {
		log.Printf("%v: failed to resolve address for world %v", client.RemoteAddr(), worldName)
		return
	}

	log.Printf("establishing connection with world %v@%v...", worldName, worldAddr)
	server, err := net.Dial("tcp", worldAddr)
	if err != nil {
		log.Printf("%v: failed to connect to server: %v", client.RemoteAddr(), err)
		return
	}
	defer server.Close()

	if _, err := server.Write([]byte(worldLine)); err != nil {
		log.Printf("%v: failed to forward world line to server: %v", client.RemoteAddr(), err)
		return
	}

	intercept := false
	xteaKey := [4]uint32{}
	inflateStream := (*C.z_stream)(nil)
	if g_RsaPublicKey != nil && g_RsaPrivateKey != nil {
		log.Printf("%v: intercepting handshake...", client.RemoteAddr())
		if xteaKey, err = InterceptGameHandshake(client, server); err != nil {
			log.Printf("%v: failed to intercept game handshake: %v", client.RemoteAddr(), err)
			return
		}

		if inflateStream, err = InflateNew(); err != nil {
			log.Printf("%v: failed to initialize inflate stream: %v", client.RemoteAddr(), err)
			return
		}

		// NOTE(fusion): Deferred statements are executed when the surrounding
		// function returns, not at the end of the enclosing scope like other
		// languages.
		defer InflateDestroy(inflateStream)

		intercept = true
	}

	log.Printf("proxy %v <-> %v now ACTIVE...", client.RemoteAddr(), server.RemoteAddr())
	wg := sync.WaitGroup{}
	wg.Add(2)

	// server -> client
	go func() {
		defer wg.Done()
		defer client.(*net.TCPConn).CloseWrite()
		for {
			data, err := ForwardNextPacket(client, server)
			if err != nil {
				if err != io.EOF {
					log.Printf("%v: server -> client error: %v", client.RemoteAddr(), err)
				}
				break
			}

			if intercept {
				var packet PlainGamePacket
				if packet, err = InterceptGamePacket(xteaKey, inflateStream, data); err != nil {
					log.Printf("%v: failed to intercept game packet: %v", client.RemoteAddr(), err)
					break
				}

				name := fmt.Sprintf("server -> client [SEQ=%08X COMPRESSED=%v PADDING=%v]",
					packet.sequence, packet.compressed, packet.padding)
				LogBuffer(name, packet.payload)
			} else {
				LogBuffer("server -> client", data)
			}
		}
	}()

	// client -> server
	go func() {
		defer wg.Done()
		defer server.(*net.TCPConn).CloseWrite()
		for {
			data, err := ForwardNextPacket(server, client)
			if err != nil {
				if err != io.EOF {
					log.Printf("%v: server <- client error: %v", client.RemoteAddr(), err)
				}
				break
			}

			if intercept {
				var packet PlainGamePacket
				if packet, err = InterceptGamePacket(xteaKey, nil, data); err != nil {
					log.Printf("%v: failed to intercept game packet: %v", client.RemoteAddr(), err)
					break
				}

				name := fmt.Sprintf("server <- client [SEQ=%08X COMPRESSED=%v PADDING=%v]",
					packet.sequence, packet.compressed, packet.padding)
				LogBuffer(name, packet.payload)
			} else {
				LogBuffer("server <- client", data)
			}
		}
	}()

	wg.Wait()
	log.Printf("proxy %v -> %v now DONE...", client.RemoteAddr(), server.RemoteAddr())
}

func RunGameProxy() {
	gameAddr := net.JoinHostPort("localhost", strconv.Itoa(g_GamePort))
	listener, err := net.Listen("tcp", gameAddr)
	if err != nil {
		log.Print(err)
		os.Exit(1)
	}

	log.Printf("game server listening on %v...", gameAddr)
	for {
		client, err := listener.Accept()
		if err != nil {
			log.Printf("failed to accept connection: %v", err)
			continue
		}

		go GameConnectionHandler(client)
	}
}

// Game Proxy
// ==============================================================================
var (
	g_ShowHelp      bool   = false
	g_HttpPort      int    = 8080
	g_GamePort      int    = 7171
	g_RsaPublicPem  string = ""
	g_RsaPrivatePem string = ""
	g_WorldOverride string = ""
)

func main() {
	var err error

	flag.BoolVar(&g_ShowHelp, "h", g_ShowHelp, "")
	flag.IntVar(&g_HttpPort, "http", g_HttpPort, "")
	flag.IntVar(&g_GamePort, "game", g_GamePort, "")
	flag.StringVar(&g_RsaPublicPem, "rsapub", g_RsaPublicPem, "")
	flag.StringVar(&g_RsaPrivatePem, "rsapriv", g_RsaPrivatePem, "")
	flag.StringVar(&g_WorldOverride, "world", g_WorldOverride, "")
	// TODO(fusion): Maybe have a way to configure `g_ServiceMap` (probably not)?
	flag.Parse()

	if g_ShowHelp {
		fmt.Println("USAGE: proxy.exe [options...]")
		flag.PrintDefaults()
		os.Exit(1)
	}

	// IMPORTANT(fusion): When intercepting, the client must be patched with a
	// known RSA key modulus (RsaPrivatePem), otherwise it should be left with
	// the original RSA key modulus for forwarding purposes.
	if g_RsaPublicPem != "" && g_RsaPrivatePem != "" {
		log.Printf("intercepting game messages (%v -> %v)...", g_RsaPrivatePem, g_RsaPublicPem)

		g_RsaPublicKey, err = RsaLoadPublicKey(g_RsaPublicPem)
		if err != nil {
			log.Printf("failed to load public key %v: %v", g_RsaPublicPem, err)
			os.Exit(1)
		}

		g_RsaPrivateKey, err = RsaLoadPrivateKey(g_RsaPrivatePem)
		if err != nil {
			log.Printf("failed to load private key %v: %v", g_RsaPrivatePem, err)
			os.Exit(1)
		}
	} else if g_RsaPublicPem != "" || g_RsaPrivatePem != "" {
		log.Printf("both `-rsapub` and `-rsapriv` need to be specified to intercept game messages...")
	}

	go RunHttpProxy()
	go RunGameProxy()

	// TODO(fusion): Probably missing a way to shutdown both proxies? Nevertheless,
	// exiting the process should kill all threads, and we don't need to be careful
	// here because there is no data to be kept.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	abortSignal := <-sigCh
	log.Printf("Received signal \"%v\", aborting...", abortSignal)
	os.Exit(0)
}
