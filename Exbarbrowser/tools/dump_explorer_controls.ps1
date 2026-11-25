# READ-ONLY: list all descendant control class names / control types inside the
# Explorer window, so we can locate the navigation pane tree control precisely.
# UI Automation read only -> rule R21 compliant.
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

$all = $win.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
Write-Output ("DESCENDANTS: {0}" -f $all.Count)
for ($i = 0; $i -lt $all.Count; $i++) {
    $c = $all.Item($i).Current
    Write-Output ("[{0}] class={1} type={2} name=[{3}] id={4}" -f $i, $c.ClassName, $c.ControlType.ProgrammaticName, $c.Name, $c.AutomationId)
}
