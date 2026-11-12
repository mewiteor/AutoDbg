# =============================================================================
# tools/agent/check_skill.ps1 —— agent 技能包的门禁（T6）
#
# 为什么要它：`tools/agent/autodbg-skill/` 是 CLI 口径下的**唯一对外契约文档**，
#   里面的 Lua 片段如果语法都不对，模型照抄就会写出跑不了的脚本（S12 复发的形态）。
# 做两件事：
#   ① 检查 `SKILL.md` 的 frontmatter（name / description 必须在，description ≤ 1024）；
#   ② 把每个 `.md` 里的 ```lua 片段抽出来，包成函数交给产物做**语法检查**
#      （只解析、不执行：片段里有 Run/Attach，真跑会要求目标）。
# 用法：pwsh tools/agent/check_skill.ps1 [-Render 64|86]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失（产物不存在）
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64'
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
$SkillDir = Join-Path $Root 'tools/agent/autodbg-skill'

if (-not (Test-Path $Exe)) { Write-Output "SKIP: 产物不存在：$Exe"; exit 2 }
if (-not (Test-Path $SkillDir)) { Write-Output "SKIP: 技能包不存在：$SkillDir"; exit 2 }

$script:pass = 0
$script:fail = 0
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_skill_check'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

function Ok([string]$n, [string]$d = '') { $script:pass++; Write-Output ("[ok]   {0}  {1}" -f $n, $d) }
function Bad([string]$n, [string]$d) { $script:fail++; Write-Output ("[FAIL] {0}  {1}" -f $n, $d) }
function Check([string]$n, [bool]$c, [string]$d = '') { if ($c) { Ok $n $d } else { Bad $n $d } }

# ---------------------------------------------------------------- ① frontmatter
$skill = Get-Content (Join-Path $SkillDir 'SKILL.md') -Raw
$fm = [regex]::Match($skill, '(?s)^---\r?\n(.*?)\r?\n---')
Check 'skill.frontmatter' $fm.Success 'SKILL.md 必须以 --- 开头的 frontmatter 开始'
if ($fm.Success) {
    $body = $fm.Groups[1].Value
    $name = [regex]::Match($body, '(?m)^name:\s*(\S+)').Groups[1].Value
    $desc = [regex]::Match($body, '(?m)^description:\s*(.+)$').Groups[1].Value
    Check 'skill.name-shape' ($name -match '^[a-z0-9]+(-[a-z0-9]+)*$' -and $name.Length -le 64) "name=$name"
    Check 'skill.description-present' ($desc.Length -gt 20) "len=$($desc.Length)"
    Check 'skill.description-max' ($desc.Length -le 1024) "len=$($desc.Length)"
}

# ---------------------------------------------------------------- ② Lua 片段语法
$mdFiles = Get-ChildItem $SkillDir -Recurse -Include *.md | Sort-Object FullName
Check 'skill.files-present' ($mdFiles.Count -ge 5) "md files=$($mdFiles.Count)"

$totalBlocks = 0
foreach ($f in $mdFiles) {
    $raw = [System.IO.File]::ReadAllText($f.FullName)
    $blocks = [regex]::Matches($raw, '```lua\r?\n(.*?)```', 'Singleline')
    if ($blocks.Count -eq 0) { continue }
    $totalBlocks += $blocks.Count

    $sb = New-Object System.Text.StringBuilder
    $i = 0
    foreach ($b in $blocks) {
        $i++
        [void]$sb.AppendLine("local function _f${i}()")
        [void]$sb.AppendLine(($b.Groups[1].Value -replace "`r", ''))
        [void]$sb.AppendLine('end')
    }
    [void]$sb.AppendLine("print('syntax-ok')")

    $lua = Join-Path $TmpDir 'check.lua'
    [System.IO.File]::WriteAllText($lua, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    $out = Join-Path $TmpDir 'check.out'
    $err = Join-Path $TmpDir 'check.err'
    $p = Start-Process -FilePath $Exe -ArgumentList @('--quiet', $lua) -RedirectStandardOutput $out `
        -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $p.WaitForExit(30000)
    $text = ((Get-Content $out -Raw -ErrorAction SilentlyContinue) + (Get-Content $err -Raw -ErrorAction SilentlyContinue))
    $rel = $f.FullName.Substring($SkillDir.Length + 1)
    Check ("skill.lua-syntax [{0}]" -f $rel) ($text -match 'syntax-ok' -and $p.ExitCode -eq 0) `
        ("blocks={0} {1}" -f $blocks.Count, (($text -replace "`r?`n", ' ').Trim()))
}
Check 'skill.lua-blocks-count' ($totalBlocks -ge 15) "total lua blocks=$totalBlocks"

Write-Output ''
Write-Output ("check_skill: pass={0} fail={1}" -f $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
