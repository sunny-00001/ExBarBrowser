# READ-ONLY: dump the Explorer navigation pane tree via MSAA (oleacc).
#
# Explorer 的导航窗格是 ProperTreeHost 里的普通 SysTreeView32。UIA 能看到树控件本身
# 但枚举不到子项；MSAA(oleacc) 能拿到全部项。本脚本额外用 accLocation 拿坐标，
# 从而按**视觉顺序**(Y 升序) + **缩进**(X) 输出 —— 这是本机 Explorer 左栏的权威基线，
# 用来和本项目 NSTC 建的树逐行比对。
#
# 合规：全程只读。不发任何合成输入(SendInput/mouse_event/SetCursorPos/合成窗口消息)，
#       符合工程规则 R21。

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

[ComImport, Guid("618736e0-3c3d-11cf-810c-00aa00389b71"),
 InterfaceType(ComInterfaceType.InterfaceIsDual)]
public interface IAccessible2 {
    [DispId(-5000)] object accParent { get; }
    [DispId(-5001)] int accChildCount { get; }
    [DispId(-5002)] object get_accChild(object childID);
    [DispId(-5003)] string get_accName(object childID);
    [DispId(-5004)] string get_accValue(object childID);
    [DispId(-5005)] string get_accDescription(object childID);
    [DispId(-5006)] object get_accRole(object childID);
    [DispId(-5007)] object get_accState(object childID);
    [DispId(-5008)] string get_accHelp(object childID);
    [DispId(-5009)] object get_accHelpContext(object childID);
    [DispId(-5010)] string get_accShortCut(object childID);
    [DispId(-5011)] object get_accFocus();
    [DispId(-5012)] object get_accSelection();
    [DispId(-5013)] string get_accDefaultAction(object childID);
    [DispId(-5014)] void accSelect(int flagsSelect, object childID);
    [DispId(-5015)] void get_accLocation(out int pxLeft, out int pyTop,
                                          out int pwWidth, out int phHeight, object childID);
    [DispId(-5016)] object accNavigate(int navDir, object start);
    [DispId(-5017)] void accDoDefaultAction(object childID);
    [DispId(-5018)] object accHitTest(int xLeft, int yTop);
}

public class NavItem {
    public string Name = "";
    public int X = 0, Y = 0, W = 0, H = 0, State = 0;
    public bool Visible = true;
    public string Role = "";
    public List<NavItem> Children = new List<NavItem>();
}

public static class MsaaTree {
    [DllImport("oleacc.dll")]
    static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint dwId, ref Guid riid,
        [MarshalAs(UnmanagedType.Interface)] out object ppvObject);

    [DllImport("oleacc.dll")]
    static extern int AccessibleChildren(IntPtr hwnd, int iChildStart, int cChildren,
        [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] object[] rgvarChildren,
        out int pcChildrenFetched);

    const uint OBJID_CLIENT = 0xFFFFFFFC;

    public static List<NavItem> Dump(IntPtr hwnd) {
        var roots = new List<NavItem>();
        Guid iid = new Guid("618736e0-3c3d-11cf-810c-00aa00389b71");
        object raw;
        int hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, ref iid, out raw);
        if (hr != 0 || raw == null) return roots;
        IAccessible2 acc = (IAccessible2)raw;
        int count = 0;
        try { count = acc.accChildCount; } catch { return roots; }
        object[] kids = new object[count];
        int fetched = 0;
        AccessibleChildren(hwnd, 0, count, kids, out fetched);
        for (int i = 0; i < fetched; i++) {
            NavItem it = Make(acc, kids[i]);
            if (it != null) roots.Add(it);
        }
        roots.Sort((a, b) => a.Y != b.Y ? a.Y.CompareTo(b.Y) : a.X.CompareTo(b.X));
        return roots;
    }

    static NavItem Make(IAccessible2 parent, object child) {
        if (child == null) return null;
        IAccessible2 sub = child as IAccessible2;
        if (sub == null) return LeafFromId(parent, child);
        NavItem it = new NavItem();
        try { it.Name = sub.get_accName(0); } catch { it.Name = "<err>"; }
        if (it.Name == null) it.Name = "";
        try { it.Role = sub.get_accRole(0).ToString(); } catch { }
        int l, t, w, h;
        try { sub.get_accLocation(out l, out t, out w, out h, 0); it.X = l; it.Y = t; it.W = w; it.H = h; }
        catch { }
        try { it.State = (int)sub.get_accState(0); } catch { }
        it.Visible = (it.State & 0x800) != 0;
        int cc = 0;
        try { cc = sub.accChildCount; } catch { }
        for (int i = 1; i <= cc; i++) {
            object c = null;
            try { c = sub.get_accChild(i); } catch { }
            if (c == null) continue;
            NavItem ch = Make(sub, c);
            if (ch != null) it.Children.Add(ch);
        }
        it.Children.Sort((a, b) => a.Y != b.Y ? a.Y.CompareTo(b.Y) : a.X.CompareTo(b.X));
        return it;
    }

    static NavItem LeafFromId(IAccessible2 parent, object idObj) {
        NavItem it = new NavItem();
        try { it.Name = parent.get_accName(idObj); } catch { it.Name = "<err>"; }
        if (it.Name == null) it.Name = "";
        try { it.Role = parent.get_accRole(idObj).ToString(); } catch { }
        int l, t, w, h;
        try { parent.get_accLocation(out l, out t, out w, out h, idObj); it.X = l; it.Y = t; it.W = w; it.H = h; }
        catch { }
        try { it.State = (int)parent.get_accState(idObj); } catch { }
        it.Visible = (it.State & 0x800) != 0;
        return it;
    }
}
"@

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$cond = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'CabinetWClass')
$wins = $AE::RootElement.FindAll($TS::Children, $cond)
$treeHwnd = [IntPtr]::Zero
$winName = ''
foreach ($w in $wins) {
    $cond2 = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'SysTreeView32')
    $trees = $w.FindAll($TS::Descendants, $cond2)
    if ($trees.Count -gt 0) {
        $h = $trees.Item(0).Current.NativeWindowHandle
        if ($h -ne 0) { $treeHwnd = [IntPtr]$h; $winName = $w.Current.Name; break }
    }
}
if ($treeHwnd -eq [IntPtr]::Zero) { Write-Output 'NO_NAV_TREE'; exit 1 }
Write-Output ("WINDOW: [{0}] treeHwnd=0x{1:X}" -f $winName, [int64]$treeHwnd)

$items = [MsaaTree]::Dump($treeHwnd)
Write-Output ("TOP_LEVEL_COUNT: {0}" -f $items.Count)

function Show($list, [int]$level) {
    foreach ($it in $list) {
        $flags = ''
        if (($it.State -band 0x10) -ne 0) { $flags += 'EXPANDED ' }
        if (($it.State -band 0x20) -ne 0) { $flags += 'COLLAPSED ' }
        if (-not $it.Visible) { $flags += 'HIDDEN ' }
        $pad = '  ' * $level
        Write-Output ("{0}- {1}   [x={2} y={3} {4}]" -f $pad, $it.Name, $it.X, $it.Y, $flags.Trim())
        if ($it.Children.Count -gt 0) { Show $it.Children ($level + 1) }
    }
}
Show $items 0
