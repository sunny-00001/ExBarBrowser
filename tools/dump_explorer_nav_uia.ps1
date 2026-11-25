# READ-ONLY: dump the navigation pane tree of the Explorer window via UI Automation.
# Locates SysTreeView32 by class name (Explorer's nav pane tree, AutomationId=100,
# ControlType=Pane - NOT Tree, which is why a ControlType filter misses it).
# Prints every visible item: depth, text, x-indent, expand state.
# UI Automation read only -> rule R21 compliant.
param(
    [string]$ClassFilter = 'SysTreeView32',
    [int]$MaxDepth = 6
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$cond = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'CabinetWClass')
$wins = $AE::RootElement.FindAll($TS::Children, $cond)
$win = $null
foreach ($w in $wins) { $win = $w; break }
if ($null -eq $win) { Write-Output 'NO_EXPLORER_WINDOW'; exit 1 }
Write-Output ("WINDOW: [{0}]" -f $win.Current.Name)

$cond2 = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, $ClassFilter)
$trees = $win.FindAll($TS::Descendants, $cond2)
Write-Output ("TREES(class={0}): {1}" -f $ClassFilter, $trees.Count)
if ($trees.Count -eq 0) { exit 2 }

$tree = $trees.Item(0)
Write-Output ("TREE: name=[{0}] id={1}" -f $tree.Current.Name, $tree.Current.AutomationId)

$script:level = 0
$walker = [System.Windows.Automation.TreeWalker]::RawViewWalker

function Walk($el) {
    if ($script:level -gt $MaxDepth) { return }
    $child = $walker.GetFirstChild($el)
    while ($null -ne $child) {
        $c = $child.Current
        $state = ''
        $ec = $null
        if ($child.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$ec)) {
            $state = $ec.Current.ExpandCollapseState.ToString()
        }
        $rect = $c.BoundingRectangle
        $pad = '  ' * $script:level
        Write-Output ("{0}[{1}] x={2} y={3} state={4}" -f $pad, $c.Name, [math]::Round($rect.X), [math]::Round($rect.Y), $state)
        $next = $walker.GetNextSibling($child)
        $script:level++
        Walk $child
        $script:level--
        $child = $next
    }
}

Walk $tree
