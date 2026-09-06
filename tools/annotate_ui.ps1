$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$src = 'C:\Users\hhh20\AppData\Local\Temp\codex-clipboard-0351fe80-eedc-4bf5-aa50-58376fd59652.png'
$out = Join-Path (Get-Location) 'annotated_ui.png'
$jsonOut = Join-Path (Get-Location) 'ui_elements.json'

$boxes = @(
    @{id='E01'; name='Left team HUD'; x=170; y=10; w=825; h=120; color='Red'},
    @{id='E02'; name='Right team HUD'; x=1550; y=10; w=840; h=120; color='Blue'},
    @{id='E03'; name='Round timer score'; x=1050; y=0; w=450; h=135; color='Gold'},
    @{id='E04'; name='Center resource HUD'; x=1115; y=118; w=340; h=128; color='Gold'},
    @{id='E05'; name='Blue energy alert'; x=1065; y=242; w=470; h=42; color='Cyan'},
    @{id='E06'; name='Left radar alert'; x=0; y=246; w=486; h=43; color='Red'},
    @{id='E07'; name='Right radar alert'; x=2074; y=246; w=485; h=43; color='Blue'},
    @{id='E08'; name='Left roster group'; x=70; y=128; w=945; h=112; color='Red'},
    @{id='E09'; name='Right roster group'; x=1225; y=128; w=1275; h=112; color='Blue'},
    @{id='E10'; name='Left unit card 1'; x=76; y=133; w=160; h=104; color='Red'},
    @{id='E11'; name='Left unit card 2'; x=232; y=133; w=165; h=104; color='Red'},
    @{id='E12'; name='Left unit card 3'; x=392; y=133; w=167; h=104; color='Red'},
    @{id='E13'; name='Left unit card 4'; x=553; y=133; w=164; h=104; color='Red'},
    @{id='E14'; name='Left unit card 5'; x=708; y=133; w=165; h=104; color='Red'},
    @{id='E15'; name='Left unit card 6'; x=862; y=133; w=158; h=104; color='Red'},
    @{id='E16'; name='Right unit card 1'; x=1244; y=133; w=164; h=104; color='Blue'},
    @{id='E17'; name='Right unit card 2'; x=1398; y=133; w=164; h=104; color='Blue'},
    @{id='E18'; name='Right unit card 3'; x=1553; y=133; w=164; h=104; color='Blue'},
    @{id='E19'; name='Right unit card 4'; x=1708; y=133; w=164; h=104; color='Blue'},
    @{id='E20'; name='Right unit card 5'; x=1864; y=133; w=164; h=104; color='Blue'},
    @{id='E21'; name='Right unit card 6'; x=2019; y=133; w=164; h=104; color='Blue'},
    @{id='E22'; name='Right unit card 7'; x=2175; y=133; w=164; h=104; color='Blue'},
    @{id='E23'; name='Right unit card 8'; x=2330; y=133; w=164; h=104; color='Blue'},
    @{id='E24'; name='Left battlefield stack'; x=145; y=560; w=400; h=355; color='Red'},
    @{id='E25'; name='Right battlefield stack'; x=1778; y=505; w=655; h=300; color='Blue'},
    @{id='E26'; name='Blue outpost status'; x=1347; y=498; w=300; h=88; color='Cyan'},
    @{id='E27'; name='Red outpost status'; x=830; y=590; w=300; h=85; color='Red'},
    @{id='E28'; name='Battlefield main'; x=0; y=286; w=2559; h=870; color='White'},
    @{id='E29'; name='Caster lower third'; x=90; y=1210; w=900; h=126; color='Gold'},
    @{id='E30'; name='Match title footer'; x=1000; y=1300; w=565; h=139; color='White'},
    @{id='E31'; name='Minimap'; x=1945; y=1080; w=610; h=359; color='Lime'},
    @{id='E32'; name='Minimap controls'; x=2485; y=1268; w=72; h=118; color='Lime'},
    @{id='E33'; name='Left red unit 1'; x=165; y=655; w=190; h=102; color='Red'},
    @{id='E34'; name='Left red unit 2'; x=92; y=764; w=185; h=110; color='Red'},
    @{id='E35'; name='Left red unit 3'; x=265; y=568; w=214; h=112; color='Red'},
    @{id='E36'; name='Right blue unit 1'; x=1810; y=548; w=285; h=95; color='Blue'},
    @{id='E37'; name='Right blue unit 2'; x=1930; y=620; w=305; h=93; color='Blue'},
    @{id='E38'; name='Right blue unit 3'; x=1995; y=700; w=190; h=100; color='Blue'}
)

$img = [System.Drawing.Bitmap]::new($src)
$g = [System.Drawing.Graphics]::FromImage($img)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$font = [System.Drawing.Font]::new('Microsoft YaHei', 14, [System.Drawing.FontStyle]::Bold)
$small = [System.Drawing.Font]::new('Microsoft YaHei', 11, [System.Drawing.FontStyle]::Bold)

function Get-Color($name) {
    switch ($name) {
        'Red' { return [System.Drawing.Color]::FromArgb(235, 235, 40, 40) }
        'Blue' { return [System.Drawing.Color]::FromArgb(235, 40, 145, 255) }
        'Gold' { return [System.Drawing.Color]::FromArgb(235, 255, 190, 30) }
        'Cyan' { return [System.Drawing.Color]::FromArgb(235, 40, 220, 255) }
        'Lime' { return [System.Drawing.Color]::FromArgb(235, 90, 230, 80) }
        default { return [System.Drawing.Color]::FromArgb(210, 255, 255, 255) }
    }
}

foreach ($b in $boxes) {
    $c = Get-Color $b.color
    $pen = [System.Drawing.Pen]::new($c, 4)
    $g.DrawRectangle($pen, $b.x, $b.y, $b.w, $b.h)
    $label = "$($b.id) $($b.name)"
    $size = $g.MeasureString($label, $small)
    $lx = [Math]::Max(0, [Math]::Min($b.x, $img.Width - $size.Width - 6))
    $ly = [Math]::Max(0, $b.y - $size.Height - 2)
    $bg = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(210, 0, 0, 0))
    $fg = [System.Drawing.SolidBrush]::new($c)
    $g.FillRectangle($bg, $lx, $ly, $size.Width + 6, $size.Height + 2)
    $g.DrawString($label, $small, $fg, $lx + 3, $ly + 1)
    $pen.Dispose(); $bg.Dispose(); $fg.Dispose()
}

$legendText = 'Red=left team  Blue=right team  Gold=match HUD  Cyan=alerts/outposts  Lime=minimap  White=field'
$legendBg = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(210, 0, 0, 0))
$legendFg = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
$g.FillRectangle($legendBg, 15, 1400, 930, 30)
$g.DrawString($legendText, $small, $legendFg, 22, 1404)
$legendBg.Dispose(); $legendFg.Dispose(); $font.Dispose(); $small.Dispose(); $g.Dispose()
$img.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$img.Dispose()

$normalized = foreach ($b in $boxes) {
    [pscustomobject]@{
        id = $b.id
        name = $b.name
        pixel = [pscustomobject]@{ x=$b.x; y=$b.y; width=$b.w; height=$b.h }
        relative = [pscustomobject]@{
            x = [Math]::Round($b.x / 2559, 6)
            y = [Math]::Round($b.y / 1439, 6)
            width = [Math]::Round($b.w / 2559, 6)
            height = [Math]::Round($b.h / 1439, 6)
        }
        percent = [pscustomobject]@{
            x = [Math]::Round($b.x / 2559 * 100, 2)
            y = [Math]::Round($b.y / 1439 * 100, 2)
            width = [Math]::Round($b.w / 2559 * 100, 2)
            height = [Math]::Round($b.h / 1439 * 100, 2)
        }
    }
}
$payload = [pscustomobject]@{ image_width=2559; image_height=1439; coordinate_origin='top-left'; boxes=$normalized }
$payload | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $jsonOut
Write-Output "Wrote $out and $jsonOut"
