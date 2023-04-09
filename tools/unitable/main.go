package main

import (
	"encoding/xml"
	"fmt"
	"hash/maphash"
	"os"
	"strconv"
)

func main() {
	if err := run(); err != nil {
		fmt.Println(err)
	}
}

func run() error {
	//http.Get(`https://www.unicode.org/Public/UCD/latest/ucdxml/ucd.nounihan.grouped.zip`)

	all, err := os.ReadFile(`ucd.nounihan.grouped.xml`)
	if err != nil {
		return err
	}

	var ucd struct {
		Repertoire struct {
			Group []struct {
				EA   string `xml:"ea,attr"`
				Char []struct {
					CP      string `xml:"cp,attr"`
					FirstCP string `xml:"first-cp,attr"`
					LastCP  string `xml:"last-cp,attr"`
					EA      string `xml:"ea,attr"`
				} `xml:"char"`
			} `xml:"group"`
		} `xml:"repertoire"`
	}

	err = xml.Unmarshal(all, &ucd)
	if err != nil {
		return err
	}

	widths := make([]byte, 0x110000)

	for _, g := range ucd.Repertoire.Group {
		for _, c := range g.Char {
			var firstCP uint64
			var lastCP uint64

			if len(c.CP) != 0 {
				firstCP, err = strconv.ParseUint(c.CP, 16, 32)
				if err != nil {
					return fmt.Errorf("unexpected cp attribute: %v", err)
				}
				lastCP = firstCP
			} else {
				firstCP, err = strconv.ParseUint(c.FirstCP, 16, 32)
				if err != nil {
					return fmt.Errorf("unexpected first-cp attribute: %v", err)
				}
				lastCP, err = strconv.ParseUint(c.LastCP, 16, 32)
				if err != nil {
					return fmt.Errorf("unexpected last-cp attribute: %v", err)
				}
			}

			ea := g.EA
			if len(c.EA) != 0 {
				ea = c.EA
			}
			var width uint8
			switch ea {
			case "N":
				width = 1
			case "Na":
				width = 1
			case "H":
				width = 1
			case "W":
				width = 2
			case "F":
				width = 2
			case "A":
				width = 0
			default:
				return fmt.Errorf("unexpected ea value")
			}

			for cp := firstCP; cp <= lastCP; cp += 1 {
				widths[cp] = width
			}
		}
	}

	//_ = os.WriteFile(`R:\out.bin`, widths, 0644)

	seed := maphash.MakeSeed()

	for tableSize := 2; tableSize <= 8192; tableSize *= 2 {
		uniqueTables := make(map[uint64]bool)

		for offset := 0; offset < len(widths); offset += tableSize {
			hash := maphash.Bytes(seed, widths[offset:offset+tableSize])
			uniqueTables[hash] = true
		}

		fmt.Printf("%4d: %4d %7d %7d %7d\n", tableSize, len(uniqueTables), len(widths)/tableSize, len(uniqueTables)*tableSize, len(uniqueTables)*tableSize+len(widths)/tableSize)
	}

	return nil
}
