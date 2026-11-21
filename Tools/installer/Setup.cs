// Manchester Ride - Windows installer and uninstaller in one small program.
// Built by Tools\installer\make_installer.bat with the C# compiler that ships with Windows (.NET Framework 4),
// so no installer toolkit has to be downloaded. The packaged game is a zip appended to this exe.
//
//   ManchesterRide-Setup.exe            install (asks where; per-user, no admin rights needed)
//   ManchesterRide-Setup.exe /silent    install to the default folder with no questions
//   ManchesterRide-Setup.exe /update    what the game runs after downloading an update from the server:
//                                       waits for the game to close, updates it where it is, starts it again
//   Uninstall.exe                       the same program without the zip: removes the game
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("Manchester Ride Setup")]
[assembly: AssemblyProduct("Manchester Ride")]
[assembly: AssemblyCompany("Manchester Riding Simulator Project")]
[assembly: AssemblyVersion("0.1.0.0")]
[assembly: AssemblyFileVersion("0.1.0.0")]

static class App
{
    public const string Name = "Manchester Ride";
    public static string Version = "0.1.0"; // replaced by the version stamped into the installer (Payload.Find)
    public const string Publisher = "Manchester Riding Simulator Project";
    public const string Key = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\ManchesterRide";
    public const string GameExe = "ManchesterRide.exe";
    public const string Marker = "mr_install.txt";        // lists what the installer put in the folder
    public const string Magic = "MRPAYLD2";

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    public static string DefaultDir { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", Name); } }
    public static string DesktopLink { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Name + ".lnk"); } }
    public static string MenuDir { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), Name); } }

    [STAThread]
    static int Main(string[] args)
    {
        try { SetProcessDPIAware(); } catch { }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        string self = Assembly.GetExecutingAssembly().Location;
        bool silent = args.Any(a => a.Equals("/silent", StringComparison.OrdinalIgnoreCase));
        bool update = args.Any(a => a.Equals("/update", StringComparison.OrdinalIgnoreCase));
        int un = Array.FindIndex(args, a => a.Equals("/uninstall", StringComparison.OrdinalIgnoreCase));
        long off, len;
        bool hasPayload = Payload.Find(self, out off, out len);
        try
        {
            if (un >= 0 || !hasPayload)
                return Uninstaller.Run(self, un >= 0 && un + 1 < args.Length ? args[un + 1] : null, silent);
            if (update)
            {
                Application.Run(new UpdateForm(self, off, len));
                return 0;
            }
            if (silent)
            {
                string dirArg = args.FirstOrDefault(a => a.StartsWith("/dir=", StringComparison.OrdinalIgnoreCase));
                var job = new InstallJob { Self = self, Offset = off, Length = len, Dir = dirArg != null ? dirArg.Substring(5).Trim('"') : DefaultDir, Desktop = true, Menu = true };
                job.Run(null);
                return 0;
            }
            Application.Run(new SetupForm(self, off, len));
            return 0;
        }
        catch (Exception e)
        {
            if (!silent) MessageBox.Show(e.Message, Name + " Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            else Console.Error.WriteLine(e);
            return 1;
        }
    }

    public static string PreviousDir()
    {
        using (var key = Registry.CurrentUser.OpenSubKey(Key))
        {
            string d = key != null ? key.GetValue("InstallLocation") as string : null;
            return d != null && Directory.Exists(d) ? d : null;
        }
    }

    public static bool GameRunning()
    {
        return Process.GetProcessesByName("ManchesterRide-Win64-Shipping").Length > 0 || Process.GetProcessesByName("ManchesterRide").Length > 0;
    }

    public static void Shortcut(string link, string target, string workDir, string icon, string description)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(link));
        Type t = Type.GetTypeFromProgID("WScript.Shell");
        object shell = Activator.CreateInstance(t);
        object s = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { link });
        Type st = s.GetType();
        st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, s, new object[] { target });
        st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, s, new object[] { workDir });
        st.InvokeMember("IconLocation", BindingFlags.SetProperty, null, s, new object[] { icon });
        st.InvokeMember("Description", BindingFlags.SetProperty, null, s, new object[] { description });
        st.InvokeMember("Save", BindingFlags.InvokeMethod, null, s, null);
        Marshal.ReleaseComObject(s); Marshal.ReleaseComObject(shell);
    }

    // Removes only what the marker file says a previous install put there.
    public static void RemoveInstalledFiles(string dir)
    {
        string marker = Path.Combine(dir, Marker);
        if (!File.Exists(marker)) return;
        foreach (string line in File.ReadAllLines(marker))
        {
            string name = line.Trim();
            if (name.Length == 0 || name.IndexOfAny(new[] { '\\', '/', ':' }) >= 0 || name == "." || name == "..") continue;
            string p = Path.Combine(dir, name);
            if (Directory.Exists(p)) Directory.Delete(p, true); else if (File.Exists(p)) File.Delete(p);
        }
        File.Delete(marker);
    }
}

// The zip sits at the end of the exe, followed by: [version block 64] magic (8 bytes), offset (8), length (8).
// Version block = "MRVER001" u32 build, version string (NUL padded): the dedicated server reads it to know
// which game version an installer holds. Older installers ("MRPAYLD1") have no version block.
static class Payload
{
    public static uint Build;

    public static bool Find(string exe, out long offset, out long length)
    {
        offset = length = 0;
        using (var f = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            if (f.Length < 24) return false;
            f.Seek(-24, SeekOrigin.End);
            var b = new byte[24]; f.Read(b, 0, 24);
            string magic = Encoding.ASCII.GetString(b, 0, 8);
            offset = BitConverter.ToInt64(b, 8); length = BitConverter.ToInt64(b, 16);
            if (offset <= 0 || length <= 0) return false;
            if (magic == "MRPAYLD1") return offset + length + 24 == f.Length;
            if (magic != App.Magic || offset + length + 64 + 24 != f.Length) return false;
            f.Seek(-24 - 64, SeekOrigin.End);
            var v = new byte[64]; f.Read(v, 0, 64);
            if (Encoding.ASCII.GetString(v, 0, 8) == "MRVER001")
            {
                Build = BitConverter.ToUInt32(v, 8);
                string ver = Encoding.ASCII.GetString(v, 12, 52).TrimEnd('\0');
                if (ver.Length > 0) App.Version = ver;
            }
            return true;
        }
    }
}

// /update: a small window with a progress bar; no questions. Started by the game, which then quits.
sealed class UpdateForm : Form
{
    readonly ProgressBar bar; readonly Label status;

    public UpdateForm(string self, long off, long len)
    {
        float k; using (var g = CreateGraphics()) k = g.DpiX / 96f;
        Text = "Updating " + App.Name;
        Icon = Icon.ExtractAssociatedIcon(self);
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = MinimizeBox = false; StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.None; BackColor = Color.White; Font = new Font("Segoe UI", 9.5f);
        ClientSize = new Size((int)(460 * k), (int)(120 * k));
        Controls.Add(new Label { Text = "Updating " + App.Name + " to version " + App.Version, Font = new Font("Segoe UI Semibold", 12f), Bounds = new Rectangle((int)(20 * k), (int)(14 * k), (int)(420 * k), (int)(28 * k)) });
        status = new Label { Text = "Waiting for the game to close...", Bounds = new Rectangle((int)(20 * k), (int)(48 * k), (int)(420 * k), (int)(20 * k)), ForeColor = Color.FromArgb(70, 70, 70) };
        bar = new ProgressBar { Bounds = new Rectangle((int)(20 * k), (int)(76 * k), (int)(420 * k), (int)(18 * k)), Maximum = 1000 };
        Controls.Add(status); Controls.Add(bar);
        Shown += delegate
        {
            var t = new Thread(() =>
            {
                string dir = App.PreviousDir() ?? App.DefaultDir;
                Exception err = null;
                try
                {
                    for (int i = 0; i < 120 && App.GameRunning(); i++) Thread.Sleep(500); // up to a minute
                    if (App.GameRunning()) throw new IOException("The game is still running. Close it, then run this update again.");
                    var job = new InstallJob { Self = self, Offset = off, Length = len, Dir = dir, Desktop = File.Exists(App.DesktopLink) || App.PreviousDir() == null, Menu = true };
                    int last = -1;
                    job.Run((f, what) =>
                    {
                        int v = (int)(f * 1000); if (v == last) return; last = v;
                        BeginInvoke((Action)(() => { bar.Value = Math.Max(0, Math.Min(1000, v)); status.Text = what + "  " + (v / 10) + "%"; }));
                    });
                }
                catch (Exception x) { err = x; }
                BeginInvoke((Action)(() =>
                {
                    if (err != null) MessageBox.Show(this, "The update could not finish.\n\n" + err.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
                    else try { Process.Start(new ProcessStartInfo(Path.Combine(dir, App.GameExe), "-JoinServer") { WorkingDirectory = dir }); } catch { }
                    Close();
                }));
            }) { IsBackground = true };
            t.Start();
        };
    }
}

sealed class SubStream : Stream
{
    readonly Stream b; readonly long start, len; long pos;
    public SubStream(Stream s, long start, long len) { b = s; this.start = start; this.len = len; }
    public override bool CanRead { get { return true; } }
    public override bool CanSeek { get { return true; } }
    public override bool CanWrite { get { return false; } }
    public override long Length { get { return len; } }
    public override long Position { get { return pos; } set { pos = value; } }
    public override void Flush() { }
    public override int Read(byte[] buf, int o, int c)
    {
        if (pos >= len) return 0;
        b.Position = start + pos;
        int n = b.Read(buf, o, (int)Math.Min(c, len - pos)); pos += n; return n;
    }
    public override long Seek(long o, SeekOrigin so) { pos = so == SeekOrigin.Begin ? o : so == SeekOrigin.Current ? pos + o : len + o; return pos; }
    public override void SetLength(long v) { throw new NotSupportedException(); }
    public override void Write(byte[] a, int o, int c) { throw new NotSupportedException(); }
}

sealed class InstallJob
{
    public string Self, Dir; public long Offset, Length; public bool Desktop, Menu;
    public volatile bool Cancel;

    /// progress(fraction 0..1, what is happening)
    public void Run(Action<double, string> progress)
    {
        Dir = Path.GetFullPath(Dir);
        Directory.CreateDirectory(Dir);
        if (progress != null) progress(0, "Removing the previous version...");
        App.RemoveInstalledFiles(Dir);

        var top = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        string root = Dir.TrimEnd('\\') + "\\";
        using (var f = new FileStream(Self, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var zip = new ZipArchive(new SubStream(f, Offset, Length), ZipArchiveMode.Read))
        {
            long total = zip.Entries.Sum(e => e.Length), done = 0;
            var drive = new DriveInfo(Path.GetPathRoot(Dir));
            if (drive.AvailableFreeSpace < total + (64L << 20))
                throw new IOException(string.Format("Not enough free space on {0} The game needs {1:N0} MB.", drive.Name, total >> 20));
            var buf = new byte[1 << 20];
            int count = 0;
            foreach (var e in zip.Entries)
            {
                if (Cancel) throw new OperationCanceledException();
                string rel = e.FullName.Replace('/', '\\');
                string dest = Path.GetFullPath(Path.Combine(Dir, rel));
                if (!dest.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue; // never write outside the folder
                top.Add(rel.Split('\\')[0]);
                // remember what we add as we go, so a cancelled or failed install can still be removed
                if ((count++ & 1023) == 0) File.WriteAllLines(Path.Combine(Dir, App.Marker), top);
                if (rel.EndsWith("\\")) { Directory.CreateDirectory(dest); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                using (var src = e.Open()) using (var dst = new FileStream(dest, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16))
                {
                    int n;
                    while ((n = src.Read(buf, 0, buf.Length)) > 0)
                    {
                        dst.Write(buf, 0, n); done += n;
                        if (progress != null) progress(total > 0 ? (double)done / total : 1, "Copying the game...");
                        if (Cancel) break;
                    }
                }
                try { File.SetLastWriteTime(dest, e.LastWriteTime.DateTime); } catch { }
            }
            if (Cancel) throw new OperationCanceledException();
        }

        if (progress != null) progress(1, "Finishing...");
        // Uninstall.exe is this program without the zip.
        string unExe = Path.Combine(Dir, "Uninstall.exe");
        using (var f = new FileStream(Self, FileMode.Open, FileAccess.Read, FileShare.Read)) using (var o = File.Create(unExe))
        {
            var b = new byte[1 << 16]; long left = Offset;
            while (left > 0) { int n = f.Read(b, 0, (int)Math.Min(b.Length, left)); if (n <= 0) break; o.Write(b, 0, n); left -= n; }
        }
        string ico = Path.Combine(Dir, "ManchesterRide.ico");
        using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("app.ico")) using (var o = File.Create(ico)) s.CopyTo(o);
        top.Add("Uninstall.exe"); top.Add("ManchesterRide.ico");
        File.WriteAllLines(Path.Combine(Dir, App.Marker), top);

        string game = Path.Combine(Dir, App.GameExe);
        if (!File.Exists(game)) throw new FileNotFoundException("The installer's game files are incomplete: " + App.GameExe + " is missing.");
        if (Desktop) App.Shortcut(App.DesktopLink, game, Dir, ico, "Open-world riding across Greater Manchester");
        else if (File.Exists(App.DesktopLink)) File.Delete(App.DesktopLink);
        if (Menu)
        {
            App.Shortcut(Path.Combine(App.MenuDir, App.Name + ".lnk"), game, Dir, ico, "Open-world riding across Greater Manchester");
            App.Shortcut(Path.Combine(App.MenuDir, "Uninstall " + App.Name + ".lnk"), unExe, Dir, unExe, "Remove " + App.Name);
        }
        else if (Directory.Exists(App.MenuDir)) Directory.Delete(App.MenuDir, true);

        long sizeKb = 0;
        try { sizeKb = new DirectoryInfo(Dir).EnumerateFiles("*", SearchOption.AllDirectories).Sum(x => x.Length) / 1024; } catch { }
        using (var k = Registry.CurrentUser.CreateSubKey(App.Key))
        {
            k.SetValue("DisplayName", App.Name); k.SetValue("DisplayVersion", App.Version); k.SetValue("Publisher", App.Publisher);
            k.SetValue("InstallLocation", Dir); k.SetValue("DisplayIcon", ico);
            k.SetValue("UninstallString", "\"" + unExe + "\""); k.SetValue("QuietUninstallString", "\"" + unExe + "\" /silent");
            k.SetValue("EstimatedSize", (int)Math.Min(int.MaxValue, sizeKb), RegistryValueKind.DWord);
            k.SetValue("NoModify", 1, RegistryValueKind.DWord); k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        }
    }
}

static class Uninstaller
{
    // Uninstall.exe lives in the folder it has to delete, so it first copies itself to the temp folder and runs from there.
    public static int Run(string self, string dirArg, bool silent)
    {
        string dir = dirArg;
        if (dir == null)
        {
            dir = Path.GetDirectoryName(self);
            if (!File.Exists(Path.Combine(dir, App.Marker)))
            {
                using (var k = Registry.CurrentUser.OpenSubKey(App.Key)) dir = k != null ? k.GetValue("InstallLocation") as string : null;
                if (dir == null || !File.Exists(Path.Combine(dir, App.Marker)))
                {
                    if (!silent) MessageBox.Show(App.Name + " is not installed.", App.Name, MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return 1;
                }
            }
            if (!silent && MessageBox.Show("Remove " + App.Name + " from this PC?\n\nYour saved profile and settings are kept.", "Uninstall " + App.Name,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return 2;
            if (App.GameRunning())
            {
                if (!silent) MessageBox.Show("The game is running. Close it, then uninstall again.", App.Name, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return 3;
            }
            string temp = Path.Combine(Path.GetTempPath(), "ManchesterRide-Uninstall.exe");
            File.Copy(self, temp, true);
            // Not waited for: the copy cannot delete this exe until this process has exited.
            Process.Start(new ProcessStartInfo(temp, "/uninstall \"" + dir.TrimEnd('\\') + "\"" + (silent ? " /silent" : "")) { UseShellExecute = false });
            return 0;
        }

        for (int i = 0; i < 20; i++) // wait for the first copy to exit and let go of its file
        {
            try { App.RemoveInstalledFiles(dir); break; } catch (Exception) { if (i == 19) throw; Thread.Sleep(300); }
        }
        try { if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); } catch { }
        try { if (File.Exists(App.DesktopLink)) File.Delete(App.DesktopLink); } catch { }
        try { if (Directory.Exists(App.MenuDir)) Directory.Delete(App.MenuDir, true); } catch { }
        Registry.CurrentUser.DeleteSubKeyTree(App.Key, false);
        if (!silent) MessageBox.Show(App.Name + " has been removed.", App.Name, MessageBoxButtons.OK, MessageBoxIcon.Information);
        return 0;
    }
}

sealed class SetupForm : Form
{
    readonly string self; readonly long off, len;
    readonly float k;
    TextBox path; CheckBox desktop, menu, launch; Button install, cancel, browse; ProgressBar bar; Label status, intro;
    InstallJob job; bool finished;

    int D(int v) { return (int)Math.Round(v * k); }

    public SetupForm(string self, long off, long len)
    {
        this.self = self; this.off = off; this.len = len;
        using (var g = CreateGraphics()) k = g.DpiX / 96f;
        Text = App.Name + " Setup";
        Icon = Icon.ExtractAssociatedIcon(self);
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.None;
        ClientSize = new Size(D(700), D(420));
        BackColor = Color.White;
        Font = new Font("Segoe UI", 9.5f);

        var side = new PictureBox { Bounds = new Rectangle(0, 0, D(240), D(420)), SizeMode = PictureBoxSizeMode.StretchImage };
        using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("side.png")) side.Image = Image.FromStream(s);
        Controls.Add(side);

        int x = D(268), w = D(404);
        Controls.Add(new Label { Text = "Install " + App.Name, Font = new Font("Segoe UI Semibold", 17f), Bounds = new Rectangle(x - D(3), D(22), w, D(40)) });
        intro = new Label { Text = "Open-world riding across Greater Manchester, with your mates.\nVersion " + App.Version + ". The game needs about " + NeedText() + " of disk space.",
            Bounds = new Rectangle(x, D(68), w, D(44)), ForeColor = Color.FromArgb(70, 70, 70) };
        Controls.Add(intro);

        Controls.Add(new Label { Text = "Install to:", Bounds = new Rectangle(x, D(126), w, D(20)) });
        path = new TextBox { Text = App.PreviousDir() ?? App.DefaultDir, Bounds = new Rectangle(x, D(148), w - D(92), D(26)) };
        browse = new Button { Text = "Browse...", Bounds = new Rectangle(x + w - D(84), D(146), D(84), D(28)) };
        browse.Click += delegate
        {
            using (var d = new FolderBrowserDialog { Description = "Choose where to install " + App.Name, SelectedPath = Directory.Exists(path.Text) ? path.Text : "" })
                if (d.ShowDialog(this) == DialogResult.OK)
                    path.Text = File.Exists(Path.Combine(d.SelectedPath, App.Marker)) ? d.SelectedPath : Path.Combine(d.SelectedPath, App.Name);
        };
        Controls.Add(path); Controls.Add(browse);

        desktop = new CheckBox { Text = "Put a shortcut on the desktop", Checked = true, Bounds = new Rectangle(x, D(194), w, D(24)) };
        menu = new CheckBox { Text = "Add to the Start menu", Checked = true, Bounds = new Rectangle(x, D(220), w, D(24)) };
        launch = new CheckBox { Text = "Start the game when setup finishes", Checked = true, Bounds = new Rectangle(x, D(246), w, D(24)) };
        Controls.Add(desktop); Controls.Add(menu); Controls.Add(launch);

        status = new Label { Text = "", Bounds = new Rectangle(x, D(296), w, D(20)), ForeColor = Color.FromArgb(70, 70, 70) };
        bar = new ProgressBar { Bounds = new Rectangle(x, D(320), w, D(18)), Maximum = 1000, Visible = false };
        Controls.Add(status); Controls.Add(bar);

        install = new Button { Text = "Install", Bounds = new Rectangle(x + w - D(204), D(368), D(98), D(32)) };
        cancel = new Button { Text = "Cancel", Bounds = new Rectangle(x + w - D(98), D(368), D(98), D(32)) };
        install.Click += OnInstall; cancel.Click += delegate { Close(); };
        Controls.Add(install); Controls.Add(cancel);
        AcceptButton = install;
    }

    string NeedText()
    {
        try
        {
            using (var f = new FileStream(self, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var zip = new ZipArchive(new SubStream(f, off, len), ZipArchiveMode.Read))
            {
                double gb = zip.Entries.Sum(e => e.Length) / 1073741824.0;
                return gb >= 1 ? gb.ToString("0.0") + " GB" : (gb * 1024).ToString("0") + " MB";
            }
        }
        catch { return "2 GB"; }
    }

    void OnInstall(object s, EventArgs e)
    {
        if (finished)
        {
            if (launch.Checked) try { Process.Start(new ProcessStartInfo(Path.Combine(job.Dir, App.GameExe)) { WorkingDirectory = job.Dir }); } catch (Exception x) { MessageBox.Show(this, x.Message, Text); }
            Close(); return;
        }
        string dir = path.Text.Trim().Trim('"');
        try { dir = Path.GetFullPath(dir); } catch { MessageBox.Show(this, "That folder name is not valid.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        if (App.GameRunning()) { MessageBox.Show(this, "The game is running. Close it first, then press Install.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        if (Directory.Exists(dir) && !File.Exists(Path.Combine(dir, App.Marker)) && Directory.EnumerateFileSystemEntries(dir).Any())
        {
            if (MessageBox.Show(this, "That folder already has other files in it:\n" + dir + "\n\nInstall there anyway?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        }
        job = new InstallJob { Self = self, Offset = off, Length = len, Dir = dir, Desktop = desktop.Checked, Menu = menu.Checked };
        path.Enabled = browse.Enabled = desktop.Enabled = menu.Enabled = install.Enabled = false;
        bar.Visible = true; status.Text = "Starting...";
        int last = -1;
        var t = new Thread(() =>
        {
            Exception err = null;
            try
            {
                job.Run((f, what) =>
                {
                    int v = (int)(f * 1000);
                    if (v == last) return; last = v;
                    BeginInvoke((Action)(() => { bar.Value = Math.Max(0, Math.Min(1000, v)); status.Text = what + "  " + (v / 10) + "%"; }));
                });
            }
            catch (Exception x) { err = x; }
            BeginInvoke((Action)(() => Done(err)));
        }) { IsBackground = true };
        t.Start();
    }

    void Done(Exception err)
    {
        if (err is OperationCanceledException) { finished = true; job = null; Close(); return; }
        if (err != null)
        {
            string msg = err is UnauthorizedAccessException ? "Windows would not let setup write to that folder. Pick a folder inside your user folder, or run setup as administrator.\n\n" + err.Message : err.Message;
            MessageBox.Show(this, "Setup could not finish.\n\n" + msg, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            path.Enabled = browse.Enabled = desktop.Enabled = menu.Enabled = install.Enabled = true;
            bar.Visible = false; status.Text = ""; job = null; return;
        }
        finished = true;
        bar.Value = 1000; status.Text = App.Name + " is installed.";
        intro.Text = "All done. " + (desktop.Checked ? "There is a " + App.Name + " icon on your desktop." : "You can start it from the Start menu.");
        install.Text = "Finish"; install.Enabled = true; cancel.Visible = false;
        install.Left = cancel.Left;
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (job != null && !finished)
        {
            if (MessageBox.Show(this, "Stop installing?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes) { job.Cancel = true; status.Text = "Stopping..."; }
            e.Cancel = true; return;
        }
        base.OnFormClosing(e);
    }
}
