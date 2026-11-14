package main

import "testing"

func TestPortalGlowPreservesTransparency(t *testing.T) {
	pixel := []byte{0, 0, 0, 0}
	paintPixel(pixel, 0, 0, 0, true)
	if pixel[3] != 0 {
		t.Fatalf("black glow became opaque: %v", pixel)
	}
	paintPixel(pixel, 32, 8, 0, true)
	want := [4]byte{255, 63, 0, 32}
	for i := range want {
		if pixel[i] != want[i] {
			t.Fatalf("glow = %v, want %v", pixel, want)
		}
	}
}

func TestPortalGlowAddsToOpaqueCore(t *testing.T) {
	pixel := []byte{200, 30, 50, 255}
	paintPixel(pixel, 80, 10, 220, true)
	want := [4]byte{255, 40, 255, 255}
	for i := range want {
		if pixel[i] != want[i] {
			t.Fatalf("composited core = %v, want %v", pixel, want)
		}
	}
}
