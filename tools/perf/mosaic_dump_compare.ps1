#requires -Version 5.1
<#
.SYNOPSIS
    Compares two mosaic dumps (KOGGER_MOSAIC_DUMP, digest_<n>.bin) tile by tile.

.DESCRIPTION
    Prints the tiles found in one dump only, and over the tiles of both: the pixels that differ (painted
    in one only, or painted in both with another value, and by how much), the heights and the marks that
    differ, and the tiles where anything does. With -Picture, also draws the most common zoom level of
    the two, tiles put together: grey where they agree, red/yellow where both painted another value
    (more/less than 16 apart), green where only the first painted, blue where only the second did.

    Exits 0 whatever it finds; the caller judges the numbers (precompute_check.ps1 does).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\mosaic_dump_compare.ps1 -A D:\Temp\on\digest_2.bin -B D:\Temp\off\digest_2.bin
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $A,
    [Parameter(Mandatory = $true)] [string] $B,
    [string] $Picture,          # a .png to draw into
    [switch] $Quiet             # only the summary line
)

$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

public class MosaicDumpTile {
    public int X, Y, Zoom, Head;
    public byte[] Image;
    public float[] Heights;
    public byte[] Marks;
}

public static class MosaicDump {
    public static Dictionary<string, MosaicDumpTile> Load(string path) {
        var tiles = new Dictionary<string, MosaicDumpTile>();
        using (var r = new BinaryReader(File.OpenRead(path))) {
            if (new string(r.ReadChars(4)) != "KMD1") throw new InvalidDataException(path + ": not a mosaic dump");
            int count = r.ReadInt32();
            for (int i = 0; i < count; ++i) {
                var t = new MosaicDumpTile();
                t.X = r.ReadInt32(); t.Y = r.ReadInt32(); t.Zoom = r.ReadInt32(); t.Head = r.ReadInt32();
                t.Image = r.ReadBytes(r.ReadInt32());
                int nh = r.ReadInt32();
                t.Heights = new float[nh];
                for (int k = 0; k < nh; ++k) t.Heights[k] = r.ReadSingle();
                t.Marks = r.ReadBytes(r.ReadInt32());
                tiles[Key(t)] = t;
            }
        }
        return tiles;
    }

    public static string Key(MosaicDumpTile t) { return t.X + "," + t.Y + "," + t.Zoom; }

    public class Result {
        public int OnlyA, OnlyB, Common, TilesDiffering;
        public long PaintedOnlyA, PaintedOnlyB, BothDiffer, MaxAbs, Heights, Marks, Pixels;
        public List<string> Lines = new List<string>();
    }

    public static Result Compare(Dictionary<string, MosaicDumpTile> a, Dictionary<string, MosaicDumpTile> b) {
        var res = new Result();
        res.OnlyA = a.Keys.Count(k => !b.ContainsKey(k));
        res.OnlyB = b.Keys.Count(k => !a.ContainsKey(k));
        foreach (var k in a.Keys.Where(b.ContainsKey).OrderBy(k => k)) {
            res.Common++;
            var ta = a[k]; var tb = b[k];
            long onlyA = 0, onlyB = 0, both = 0, maxAbs = 0, h = 0, m = 0;
            int n = Math.Min(ta.Image.Length, tb.Image.Length);
            res.Pixels += n;
            for (int p = 0; p < n; ++p) {
                int va = ta.Image[p], vb = tb.Image[p];
                if (va == vb) continue;
                if (va == 0) onlyB++;
                else if (vb == 0) onlyA++;
                else { both++; maxAbs = Math.Max(maxAbs, Math.Abs(va - vb)); }
            }
            for (int i = 0; i < Math.Min(ta.Heights.Length, tb.Heights.Length); ++i)
                if (Math.Abs(ta.Heights[i] - tb.Heights[i]) > 1e-4f) h++;
            for (int i = 0; i < Math.Min(ta.Marks.Length, tb.Marks.Length); ++i)
                if (ta.Marks[i] != tb.Marks[i]) m++;
            res.PaintedOnlyA += onlyA; res.PaintedOnlyB += onlyB; res.BothDiffer += both;
            res.MaxAbs = Math.Max(res.MaxAbs, maxAbs); res.Heights += h; res.Marks += m;
            if (onlyA + onlyB + both + h + m > 0 || ta.Head != tb.Head || ta.Image.Length != tb.Image.Length) {
                res.TilesDiffering++;
                res.Lines.Add(string.Format("  tile {0}: head {1}/{2}, painted only in A {3}, only in B {4}, other value {5} (max {6} apart), heights {7}, marks {8}",
                    k, ta.Head, tb.Head, onlyA, onlyB, both, maxAbs, h, m));
            }
        }
        return res;
    }

    public static string Draw(Dictionary<string, MosaicDumpTile> a, Dictionary<string, MosaicDumpTile> b, string png) {
        var common = a.Keys.Where(b.ContainsKey).Select(k => a[k]).ToList();
        if (common.Count == 0) return "no common tiles to draw";
        int zoom = common.GroupBy(t => t.Zoom).OrderByDescending(g => g.Count()).First().Key;
        var tiles = common.Where(t => t.Zoom == zoom).ToList();
        int side = (int)Math.Round(Math.Sqrt(tiles[0].Image.Length));
        int x0 = tiles.Min(t => t.X), x1 = tiles.Max(t => t.X), y0 = tiles.Min(t => t.Y), y1 = tiles.Max(t => t.Y);
        int w = (x1 - x0 + 1) * side, hgt = (y1 - y0 + 1) * side;
        using (var bmp = new System.Drawing.Bitmap(w, hgt, System.Drawing.Imaging.PixelFormat.Format24bppRgb)) {
            var rect = new System.Drawing.Rectangle(0, 0, w, hgt);
            var data = bmp.LockBits(rect, System.Drawing.Imaging.ImageLockMode.WriteOnly, bmp.PixelFormat);
            var buf = new byte[data.Stride * hgt];
            foreach (var ta in tiles) {
                var tb = b[Key(ta)];
                if (tb.Image.Length != ta.Image.Length) continue;
                int ox = (ta.X - x0) * side, oy = (y1 - ta.Y) * side;
                for (int p = 0; p < ta.Image.Length; ++p) {
                    int va = ta.Image[p], vb = tb.Image[p];
                    int px = ox + p % side, py = oy + side - 1 - p / side;
                    byte cr, cg, cb;
                    if (va == vb) { cr = cg = cb = (byte)(va / 3); }
                    else if (va == 0) { cr = 0; cg = 120; cb = 255; }
                    else if (vb == 0) { cr = 0; cg = 255; cb = 0; }
                    else if (Math.Abs(va - vb) > 16) { cr = 255; cg = 40; cb = 40; }
                    else { cr = 255; cg = 200; cb = 0; }
                    int o = py * data.Stride + px * 3;
                    buf[o] = cb; buf[o + 1] = cg; buf[o + 2] = cr;
                }
            }
            System.Runtime.InteropServices.Marshal.Copy(buf, 0, data.Scan0, buf.Length);
            bmp.UnlockBits(data);
            bmp.Save(png, System.Drawing.Imaging.ImageFormat.Png);
        }
        return string.Format("picture {0}: zoom {1}, tiles x {2}..{3}, y {4}..{5}, {6}x{7} px", png, zoom, x0, x1, y0, y1, w, hgt);
    }
}
'@

$da = [MosaicDump]::Load((Resolve-Path $A).Path)
$db = [MosaicDump]::Load((Resolve-Path $B).Path)
$r = [MosaicDump]::Compare($da, $db)
"tiles: {0} in both, {1} only in A, {2} only in B; of {3} pixels: painted only in A {4}, only in B {5}, another value {6} (at most {7} apart); heights {8}, marks {9}; tiles that differ {10}" -f `
    $r.Common, $r.OnlyA, $r.OnlyB, $r.Pixels, $r.PaintedOnlyA, $r.PaintedOnlyB, $r.BothDiffer, $r.MaxAbs, $r.Heights, $r.Marks, $r.TilesDiffering
if (-not $Quiet) { $r.Lines }
if ($Picture) { [MosaicDump]::Draw($da, $db, [System.IO.Path]::GetFullPath($Picture)) }
