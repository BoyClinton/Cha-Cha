# Compares a reference screenshot (e.g. Cemu's F12 capture, 1920x1080 TV image) with a frame dump of the port
# (MH3U_DUMP_FRAME -> build/frame_N.bmp): prints mean absolute difference, PSNR and the share of pixels differing by
# more than 24 in any channel, and writes <Out>_diff.png (difference x4, red) and <Out>_side.png (half-size side by
# side). Outputs are game imagery: keep them under build/, never in git.
# Usage:
#   scripts/compare-frames.ps1 -Reference build/compare/cemu_5_menu.png -Port build/frame_1290.bmp -Out build/compare/menu
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Reference,
    [Parameter(Mandatory = $true)] [string]$Port,
    [Parameter(Mandatory = $true)] [string]$Out
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("FrameCompare" -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class FrameCompare {
  static byte[] Pixels(Bitmap b, out int stride) {
    var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
    stride = d.Stride; var a = new byte[stride * b.Height]; Marshal.Copy(d.Scan0, a, 0, a.Length); b.UnlockBits(d); return a;
  }
  public static string Diff(Bitmap a, Bitmap b, string heatPath) {
    int w = Math.Min(a.Width, b.Width), h = Math.Min(a.Height, b.Height), sa, sb;
    var pa = Pixels(a, out sa); var pb = Pixels(b, out sb);
    var heat = new Bitmap(w, h, PixelFormat.Format24bppRgb);
    var hd = heat.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
    var ho = new byte[hd.Stride * h];
    double sum = 0, sq = 0; long big = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      int m = 0;
      for (int c = 0; c < 3; c++) { int d = Math.Abs(pa[y * sa + x * 3 + c] - pb[y * sb + x * 3 + c]); sum += d; sq += d * d; if (d > m) m = d; }
      if (m > 24) big++;
      byte v = (byte)Math.Min(255, m * 4);
      ho[y * hd.Stride + x * 3 + 2] = v; ho[y * hd.Stride + x * 3 + 1] = (byte)(v / 3);
    }
    Marshal.Copy(ho, 0, hd.Scan0, ho.Length); heat.UnlockBits(hd); heat.Save(heatPath);
    double n = (double)w * h * 3, mse = sq / n;
    return String.Format("mean {0:F2}  psnr {1:F1} dB  pixels>24: {2:F2}%", sum / n, mse > 0 ? 10 * Math.Log10(255 * 255 / mse) : 99, 100.0 * big / (w * h));
  }
}
"@
}
$a = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Reference).Path)
$b = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Port).Path)
$outFull = [System.IO.Path]::GetFullPath($Out)
[FrameCompare]::Diff($a, $b, "${outFull}_diff.png")
$s = New-Object System.Drawing.Bitmap 1920, 540
$g = [System.Drawing.Graphics]::FromImage($s)
$g.DrawImage($a, 0, 0, 960, 540)
$g.DrawImage($b, 960, 0, 960, 540)
$f = New-Object System.Drawing.Font 'Arial', 18
$g.DrawString('reference', $f, [System.Drawing.Brushes]::Yellow, 8, 8)
$g.DrawString('port', $f, [System.Drawing.Brushes]::Yellow, 968, 8)
$s.Save("${outFull}_side.png")
$a.Dispose(); $b.Dispose()
