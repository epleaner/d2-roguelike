// SPDX-License-Identifier: GPL-3.0-or-later
// Render the approved PP portal frame using OpenDiablo2's GPL-3.0 DCC/COF decoder.
package main

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"

	"github.com/OpenDiablo2/OpenDiablo2/d2common/d2enum"
	"github.com/OpenDiablo2/OpenDiablo2/d2common/d2fileformats/d2cof"
	"github.com/OpenDiablo2/OpenDiablo2/d2common/d2fileformats/d2dcc"
)

const selectedFrame = 4

func loadDCC(path string) (*d2dcc.DCCDirection, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	animation, err := d2dcc.Load(data)
	if err != nil {
		return nil, err
	}
	if len(animation.Directions) != 1 || len(animation.Directions[0].Frames) <= selectedFrame {
		return nil, fmt.Errorf("unexpected portal animation layout: %s", path)
	}
	direction := animation.Directions[0]
	if len(direction.Frames[selectedFrame].PixelData) != direction.Box.Width*direction.Box.Height {
		return nil, fmt.Errorf("unexpected portal frame dimensions: %s", path)
	}
	return direction, nil
}

func paintPixel(dst []byte, r, g, b byte, additive bool) {
	if !additive {
		dst[0], dst[1], dst[2], dst[3] = r, g, b, 255
		return
	}
	if dst[3] != 0 {
		dst[0] = uint8(min(255, int(dst[0])+int(r)))
		dst[1] = uint8(min(255, int(dst[1])+int(g)))
		dst[2] = uint8(min(255, int(dst[2])+int(b)))
		return
	}
	alpha := max(r, g, b)
	if alpha == 0 {
		return
	}
	dst[0] = uint8(int(r) * 255 / int(alpha))
	dst[1] = uint8(int(g) * 255 / int(alpha))
	dst[2] = uint8(int(b) * 255 / int(alpha))
	dst[3] = alpha
}

func paint(canvas *image.NRGBA, direction *d2dcc.DCCDirection, palette []byte, left, top int, additive bool) {
	frame := direction.Frames[selectedFrame]
	for y := range direction.Box.Height {
		for x := range direction.Box.Width {
			index := int(frame.PixelData[y*direction.Box.Width+x])
			if index == 0 {
				continue
			}
			r, g, b := palette[index*3+2], palette[index*3+1], palette[index*3]
			px := canvas.PixOffset(direction.Box.Left-left+x, direction.Box.Top-top+y)
			paintPixel(canvas.Pix[px:px+4], r, g, b, additive)
		}
	}
}

func render(source, output string) error {
	palette, err := os.ReadFile(filepath.Join(source, "pal.dat"))
	if err != nil {
		return err
	}
	if len(palette) != 768 {
		return fmt.Errorf("unexpected Act I palette size")
	}
	data, err := os.ReadFile(filepath.Join(source, "pponhth.cof"))
	if err != nil {
		return err
	}
	cof, err := d2cof.Unmarshal(data)
	if err != nil {
		return err
	}
	if cof.NumberOfLayers != 2 || cof.NumberOfDirections != 1 || cof.FramesPerDirection <= selectedFrame ||
		cof.Priority[0][selectedFrame][0] != d2enum.CompositeTypeTorso ||
		cof.Priority[0][selectedFrame][1] != d2enum.CompositeTypeHead {
		return fmt.Errorf("unexpected portal composition")
	}
	head, err := loadDCC(filepath.Join(source, "pphdlitonhth.dcc"))
	if err != nil {
		return err
	}
	torso, err := loadDCC(filepath.Join(source, "pptrlitonhth.dcc"))
	if err != nil {
		return err
	}
	left, top := min(head.Box.Left, torso.Box.Left), min(head.Box.Top, torso.Box.Top)
	right := max(head.Box.Left+head.Box.Width, torso.Box.Left+torso.Box.Width)
	bottom := max(head.Box.Top+head.Box.Height, torso.Box.Top+torso.Box.Height)
	canvas := image.NewNRGBA(image.Rect(0, 0, right-left, bottom-top))
	paint(canvas, torso, palette, left, top, false)
	paint(canvas, head, palette, left, top, true)
	file, err := os.Create(output)
	if err != nil {
		return err
	}
	if err := png.Encode(file, canvas); err != nil {
		file.Close()
		return err
	}
	return file.Close()
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: portal_art <extracted-inputs> <output.png>")
		os.Exit(2)
	}
	if err := render(os.Args[1], os.Args[2]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
