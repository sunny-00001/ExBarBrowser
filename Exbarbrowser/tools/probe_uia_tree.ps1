# READ-ONLY: probe several ways to enumerate the items of a SysTreeView32 nav tree
# via UI Automation. Explorer exposes its nav tree through the shell's
# MSAA->UIA proxy, so try: RawView / ControlView / FindAll(Children) /
# LegacyIAccessible pattern.
# UI Automation read only -> rule R21 compliant.
param(
    [string]$WinClass = 'CabinetWClass',
    [string]$TreeClass = 'SysTreeView32'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$cond = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, $WinClass)
$wins = $AE::RootElement.FindAll($TS::Children, $cond)
$win = $null
foreach ($w in $wins) { $win = $w; break }
if ($null -eq $win) { Write-Output 'NO_WINDOW'; exit 1 }
Write-Output ("WINDOW: [{0}] class={1}" -f $win.Current.Name, $win.Current.ClassName)

$cond2 = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, $TreeClass)
$trees = $win.FindAll($TS::Descendants, $cond2)
Write-Output ("TREES: {0}" -f $trees.Count)
if ($trees.Count -eq 0) { exit 2 }
$tree = $trees.Item(0)
Write-Output ("TREE: name=[{0}] id={1} offscreen={2}" -f $tree.Current.Name, $tree.Current.AutomationId, $tree.Current.IsOffscreen)

# 1) FindAll(Children) on the tree
$kids = $tree.FindAll($TS::Children, [System.Windows.Automation.Condition]::TrueCondition)
Write-Output ("FindAll(Children).Count = {0}" -f $kids.Count)

# 2) RawViewWalker
$rw = [System.Windows.Automation.TreeWalker]::RawViewWalker
$c = $rw.GetFirstChild($tree)
$n = 0
while ($null -ne $c -and $n -lt 5) { Write-Output ("  raw: [{0}] class={1}" -f $c.Current.Name, $c.Current.ClassName); $c = $rw.GetNextSibling($c); $n++ }
Write-Output ("RawViewWalker children seen = {0}" -f $n)

# 3) ControlViewWalker
$cw = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$c = $cw.GetFirstChild($tree)
$n = 0
while ($null -ne $c -and $n -lt 5) { Write-Output ("  ctl: [{0}] class={1}" -f $c.Current.Name, $c.Current.ClassName); $c = $cw.GetNextSibling($c); $n++ }
Write-Output ("ControlViewWalker children seen = {0}" -f $n)

# 4) LegacyIAccessible pattern on the tree
$legacy = $null
if ($tree.TryGetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern, [ref]$legacy)) {
    Write-Output ("LegacyIAccessible role={0} childCount={1}" -f $legacy.Current.Role, $legacy.Current.ChildCount)
} else {
    Write-Output 'LegacyIAccessible pattern: NOT SUPPORTED on tree'
}

# 5) FindAll(Descendants) of the whole window with ControlType TreeItem
$ti = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::TreeItem)
$items = $win.FindAll($TS::Descendants, $ti)
Write-Output ("TreeItems in window = {0}" -f $items.Count)
for ($i = 0; $i -lt [Math]::Min($items.Count, 200); $i++) {
    $c = $items.Item($i).Current
    $rect = $c.BoundingRectangle
    Write-Output ("  [{0}] x={1} y={2}" -f $c.Name, [math]::Round($rect.X), [math]::Round($rect.Y))
}
