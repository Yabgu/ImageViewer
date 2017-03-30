using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using System.Collections;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace ImageViewer
{
    public partial class frmMain : Form
    {
        private ResizeControl ResizeControl;
        private CropControl CropControl;
        bool IsImageManipulated = false;
        private System.Drawing.Bitmap MyBitmap { get; set; }

        private int nSelectedFile = -1;
        private string szRequestedFile;
        private ArrayList FileArray;
        private ArrayList FileTypes;
        public string szCurrentDirectory;
        bool IsFullScreen = false;

        static class AutoDisposeQueue
        {
            static Queue<IDisposable> DisposeList = new Queue<IDisposable>();
            static AutoResetEvent Event = new AutoResetEvent(false);
            static bool QuitRequested = false;

            public static void Run()
            {
                QuitRequested = false;
                while (true)
                {
                    try
                    {
                        Event.WaitOne();
                        if (QuitRequested)
                            return;
                        while (DisposeList.Count > 0)
                        {
                            DisposeList.Dequeue().Dispose();
                        }
                    }
                    catch (Exception) { }
                }
            }

            public static void Stop()
            {
                QuitRequested = true;
                Event.Set();
            }

            public static void Enqueue(IDisposable o)
            {
                DisposeList.Enqueue(o);
                Event.Set();
            }
        }

        private void ProcessDirectory()
        {
            string[] szFiles;
            FileArray = new ArrayList();

            // find image files
            foreach (string szType in FileTypes)
            {
                szFiles = Directory.GetFiles(Directory.GetCurrentDirectory(), szType);
                if (szFiles.Length > 0)
                    FileArray.AddRange(szFiles);
            }
            if (FileArray.Count > 0)
            {
                if (szRequestedFile != null && szRequestedFile.ToString() != "")
                {
                    nSelectedFile = FileArray.IndexOf(szRequestedFile);
                }
            }
            else
            {
                nSelectedFile = -1;
            }
        }

        private string GetFilePath()
        {
            if (nSelectedFile > FileArray.Count) nSelectedFile = 0;
            return FileArray[nSelectedFile].ToString();
        }

        void SetTitle()
        {
            this.Text = GetFilePath() + " - ImageViewer";
        }

        private void SetPicture()
        {
            try
            {
                SetTitle();

                if (MyBitmap != null)
                {
                    try { MyBitmap.Dispose(); }
                    catch (Exception) { }
                }
                MyBitmap = new Bitmap(GetFilePath());
                imageRenderer1.SelectedImage = MyBitmap;
                IsImageManipulated = false;
            }
            catch (Exception ex)
            {
                // tslStatus.Text = "ERROR: " + ex.ToString();
            }
        }

        void InitializeViewer()
        {
            if (!this.DesignMode)
            {
                if (MyBitmap != null)
                {
                    Task.Run(() =>
                    {
                        try
                        {
                            var bits = ImageRenderer.GetBitmapBits(MyBitmap);
                            this.imageRenderer1.Load += (s, e) =>
                                {
                                    try
                                    {
                                        this.imageRenderer1.SetBitmap(MyBitmap, bits);
                                    }
                                    catch (Exception)
                                    {
                                    }
                                    finally
                                    {
                                        ImageRenderer.FreeBitmapBits(MyBitmap, bits);
                                    }
                                };
                        }
                        catch (Exception)
                        {
                        }
                    });
                }

                this.MouseWheel += frmMain_MouseWheel;
                this.imageRenderer1.KeyDown += imageRenderer1_KeyDown;
                this.imageRenderer1.KeyUp += imageRenderer1_KeyUp;
                this.imageRenderer1.MouseDown += imageRenderer1_MouseDown;
                this.imageRenderer1.MouseUp += imageRenderer1_MouseUp;
                this.imageRenderer1.MouseMove += imageRenderer1_MouseMove;
                this.imageRenderer1.RunOperationRequested += imageRenderer1_RunOperationRequested;
                this.imageRenderer1.AllowDrop = true;
                this.imageRenderer1.DragEnter += imageRenderer1_DragEnter;
                this.imageRenderer1.DragDrop += imageRenderer1_DragDrop;
            }
        }

        void imageRenderer1_DragEnter(object sender, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
                e.Effect = DragDropEffects.Copy;
        }

        void imageRenderer1_DragDrop(object sender, DragEventArgs e)
        {
            string[] files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files.Length >= 1)
            {
                szRequestedFile = files[0];
                Directory.SetCurrentDirectory(Path.GetDirectoryName(szRequestedFile));
                ProcessDirectory();
                SetPicture();
            }
        }

        void imageRenderer1_RunOperationRequested(object sender, EventArgs e)
        {
            Operation_OkClicked(sender, e);
        }



        public frmMain(string[] args)
        {
            // initialize the image types array
            FileTypes = new ArrayList();
            FileTypes.Add("*.JPG");
            FileTypes.Add("*.JPEG");
            FileTypes.Add("*.GIF");
            FileTypes.Add("*.BMP");
            FileTypes.Add("*.PNG");
            FileTypes.Add("*.TIF");
            FileTypes.Add("*.TIFF");

            if (args.Length > 0)
            {
                szRequestedFile = args[args.Length - 1];
                Directory.SetCurrentDirectory(Path.GetDirectoryName(szRequestedFile));
                // process current folder
                ProcessDirectory();
                try
                {
                    if (nSelectedFile != -1)
                        MyBitmap = new Bitmap(GetFilePath());
                }
                catch (Exception) { }
            }
            InitializeComponent();
            InitializeViewer();
        }

        private void tsbClose_Click(object sender, EventArgs e)
        {
            this.Close();
        }

        private void tsbOpenImage_Click(object sender, EventArgs e)
        {
            if (openFileDialog1.ShowDialog() == DialogResult.OK)
            {
                szRequestedFile = openFileDialog1.FileName;
                Directory.SetCurrentDirectory(Directory.GetParent(szRequestedFile).FullName.ToString());
                ProcessDirectory();
                if (nSelectedFile != -1)
                    SetPicture();
            }

        }

        private void frmMain_Load(object sender, EventArgs e)
        {
            try
            {
                if (this.DesignMode)
                    return;

                ResizeControl = new ResizeControl();
                ResizeControl.Dock = DockStyle.Right;
                ResizeControl.ValueChanged += Resize_ValueChanged;
                ResizeControl.OkClicked += Operation_OkClicked;
                ResizeControl.CancelClicked += Operation_CancelClicked;

                CropControl = new ImageViewer.CropControl();
                CropControl.Dock = DockStyle.Right;
                CropControl.ValueChanged += CropControl_ValueChanged;
                CropControl.OkClicked += Operation_OkClicked;
                CropControl.CancelClicked += Operation_CancelClicked;
                imageRenderer1.SelectionRectangleUpdate += imageRenderer1_SelectionRectangleUpdate;

                string all = "All Graphics Types|*.bmp;*.jpg;*.jpeg;*.png;*.tif;*.tiff|";
                string some = "BMP|*.bmp|GIF|*.gif|JPG|*.jpg;*.jpeg|PNG|*.png|TIFF|*.tif;*.tiff";
                openFileDialog1.Filter = all + some;
                saveFileDialog1.Filter = some;

                backgroundWorker1.DoWork += (s, _e) => { AutoDisposeQueue.Run(); };
                this.FormClosing += (s, _e) => { AutoDisposeQueue.Stop(); };
                backgroundWorker1.RunWorkerAsync();
            }
            catch (Exception ex)
            {
                //tslStatus.Text = "ERROR: " + ex.ToString();
            }
        }

        void imageRenderer1_SelectionRectangleUpdate(object sender, ImageRenderer.SelectionUpdateEventArgs e)
        {
            CropControl.SetRectangle(e.ToRectangle(), MyBitmap.Size);
        }

        void CropControl_ValueChanged(object sender, CropControl.CropControlValueChangedArgs e)
        {
            if (!imageRenderer1.UpdateSelection(e.dx, e.dy, e.ResizeMotivation))
                CropControl.SetRectangle(imageRenderer1.SelectionRect, MyBitmap.Size);
        }

        public delegate void EndOperation();

        EndOperation OnOperationOk = null;
        EndOperation OnOperationFinish = null;

        void Operation_CancelClicked(object sender, EventArgs e)
        {
            OnOperationOk = null;

            if (imageRenderer1.SelectedImage != MyBitmap)
            {
                var toDispose = imageRenderer1.SelectedImage;
                imageRenderer1.SelectedImage = MyBitmap;
                AutoDisposeQueue.Enqueue(toDispose);
            }
            if (OnOperationFinish != null)
                OnOperationFinish();

            OnOperationFinish = null;
        }

        void Operation_OkClicked(object sender, EventArgs e)
        {
            if (OnOperationOk != null)
                OnOperationOk();

            OnOperationOk = null;

            if (imageRenderer1.SelectedImage != MyBitmap)
            {
                var toDispose = MyBitmap;

                if (imageRenderer1.SelectedImage == MyBitmap)
                    return;

                IsImageManipulated = true;
                MyBitmap = imageRenderer1.SelectedImage;
                AutoDisposeQueue.Enqueue(toDispose);
            }

            if (OnOperationFinish != null)
                OnOperationFinish();
        }


        bool IsDragging = false;
        Point PreviousMousePos = Point.Empty;

        void imageRenderer1_MouseMove(object sender, MouseEventArgs e)
        {
            if (IsDragging)
            {
                int dx, dy;
                dx = e.Location.X - PreviousMousePos.X;
                dy = e.Location.Y - PreviousMousePos.Y;
                imageRenderer1.Move(dx, dy);
                PreviousMousePos = e.Location;
            }
        }

        void imageRenderer1_MouseUp(object sender, MouseEventArgs e)
        {
            imageRenderer1.Capture = false;
            IsDragging = false;
            if (MyBitmap != null && e.Button == System.Windows.Forms.MouseButtons.Right)
            {
                if (!IsOperationPending)
                    contextMenuStrip1.Show(new Point(MousePosition.X, MousePosition.Y));
            }
        }

        void imageRenderer1_MouseDown(object sender, MouseEventArgs e)
        {
            if (e.Button == System.Windows.Forms.MouseButtons.Middle || e.Button == System.Windows.Forms.MouseButtons.Left)
            {
                PreviousMousePos = e.Location;
                imageRenderer1.Capture = true;
                IsDragging = true;
            }
        }

        List<Keys> PressedKeys = new List<Keys>();

        void imageRenderer1_KeyDown(object sender, KeyEventArgs e)
        {
            foreach (var key in PressedKeys)
            {
                if (key == e.KeyCode)
                    return;
            }

            PressedKeys.Add(e.KeyCode);
        }

        void SetFullscreen()
        {
            if (!IsFullScreen)
            {
                // TODO Save window position here
                this.SuspendDraw();
                try
                {
                    // TODO: This is not true way to make window fullscreen
                    this.TopMost = true;
                    this.toolBar.Visible = false;
                    this.WindowState = FormWindowState.Normal;
                    this.FormBorderStyle = FormBorderStyle.None;
                    this.WindowState = FormWindowState.Maximized;

                    this.IsFullScreen = true;
                }
                finally
                {
                    this.ResumeDraw();
                    this.Invalidate();
                }
            }
        }

        void SetWindowed()
        {
            if (IsFullScreen)
            {
                // TODO Restore window position here
                this.TopMost = false;
                this.FormBorderStyle = FormBorderStyle.Sizable;

                this.toolBar.Visible = true;
                this.IsFullScreen = false;
            }
        }

        void imageRenderer1_KeyUp(object sender, KeyEventArgs e)
        {
            if (e.KeyCode == Keys.F)
            {

                if (this.IsFullScreen)
                {
                    SetWindowed();
                }
                else
                {
                    SetFullscreen();
                }
            }

            foreach (var key in PressedKeys)
            {
                if (key == e.KeyCode)
                {
                    PressedKeys.Remove(key);
                    break;
                }
            }

            switch (e.KeyCode)
            {
                case Keys.Add:
                    imageRenderer1.ZoomIn(25);
                    break;
                case Keys.Subtract:
                    imageRenderer1.ZoomOut(25);
                    break;
                case Keys.Escape:
                    if (this.IsOperationPending)
                        this.Operation_CancelClicked(this, null);
                    else
                        this.Close();
                    break;
            }
        }

        double xSpeed = 0.0;
        double ySpeed = 0.0;

        private void timer1_Tick(object sender, EventArgs e)
        {
            int dx = 0;
            int dy = 0;
            foreach (var key in PressedKeys)
            {
                switch (key)
                {
                    case Keys.Up:
                        ++dy;
                        break;
                    case Keys.Down:
                        --dy;
                        break;
                    case Keys.Left:
                        ++dx;
                        break;
                    case Keys.Right:
                        --dx;
                        break;
                }
            }
            if (dx == 0 && dy == 0)
            {
                xSpeed = ySpeed = 0;
            }
            else
            {
                xSpeed += dx * 5;
                ySpeed += dy * 5;
                imageRenderer1.Move(xSpeed, ySpeed);
                xSpeed *= 0.9; ySpeed *= 0.9;
            }

            try
            {
                if (!IsOperationPending)
                {
                    SeamCarver = null;
                }
                else if (SeamCarver != null)
                {
                    if (HeightDiffOfSeamCarver > 0)
                    {
                        SeamCarver.AddHorizontalSeam();
                        --HeightDiffOfSeamCarver;
                        imageRenderer1.SelectedImage = SeamCarver.GetImage();
                    }
                    else if (HeightDiffOfSeamCarver < 0)
                    {
                        SeamCarver.RemoveHorizontalSeam();
                        ++HeightDiffOfSeamCarver;
                        imageRenderer1.SelectedImage = SeamCarver.GetImage();
                    }


                    if (WidthDiffOfSeamCarver > 0)
                    {
                        SeamCarver.AddVerticalSeam();
                        --WidthDiffOfSeamCarver;
                        imageRenderer1.SelectedImage = SeamCarver.GetImage();
                    }
                    else if (WidthDiffOfSeamCarver < 0)
                    {
                        SeamCarver.RemoveVerticalSeam();
                        ++WidthDiffOfSeamCarver;
                        imageRenderer1.SelectedImage = SeamCarver.GetImage();
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine(ex.ToString());
            }
        }

        ImageViewer.Effects.SeamCarver SeamCarver = null;
        int WidthDiffOfSeamCarver = 0;
        int HeightDiffOfSeamCarver = 0;

        void Resize_ValueChanged(object sender, ResizeControl.SizeEventArgs e)
        {
            var toDispose = imageRenderer1.SelectedImage;

            if (e.SeamCarving)
            {
                int w = e.Width, h = e.Height;
                int dw = e.Width - imageRenderer1.SelectedImage.Width, dh = e.Height - imageRenderer1.SelectedImage.Height;

                Int64[] sw = new Int64[MyBitmap.Width];
                Int64[] sh = new Int64[MyBitmap.Height];

                if (SeamCarver == null)
                {
                    SeamCarver = new Effects.SeamCarver(MyBitmap);
                }
                else if (dw > 0 || dh > 0)
                {
                    dw = e.Width - MyBitmap.Width;
                    dh = e.Height - MyBitmap.Height;
                    SeamCarver = new Effects.SeamCarver(MyBitmap);
                    imageRenderer1.SelectedImage = MyBitmap;
                }
                WidthDiffOfSeamCarver = dw;
                HeightDiffOfSeamCarver = dh;
            }
            else
            {
                var tmpBitmap = new Bitmap(e.Width, e.Height, MyBitmap.PixelFormat);

                Graphics g = Graphics.FromImage(tmpBitmap);
                g.DrawImage(MyBitmap, new Rectangle(0, 0, e.Width, e.Height), new Rectangle(0, 0, MyBitmap.Width, MyBitmap.Height), GraphicsUnit.Pixel);
                g.Flush();
                g.Dispose();
                imageRenderer1.SelectedImage = tmpBitmap;
                SeamCarver = null;
            }

            if (toDispose != MyBitmap)
                AutoDisposeQueue.Enqueue(toDispose);


        }

        bool AskSaveChanges()
        {
            return MessageBox.Show(this, "Do you want to save changes?", "You ara about to change picture!",
                MessageBoxButtons.YesNo, MessageBoxIcon.Exclamation) == System.Windows.Forms.DialogResult.Yes;
        }

        void SeekFile(int sc)
        {
            if (IsImageManipulated && AskSaveChanges())
            {
                this.tsbSaveAs_Click(this, null);
                return;
            }

            if (nSelectedFile != -1)
            {
                nSelectedFile -= sc;
                if (nSelectedFile < 0)
                {
                    nSelectedFile = FileArray.Count - 1;
                }
                if (nSelectedFile >= FileArray.Count)
                {
                    nSelectedFile = 0;
                }
                SetPicture();
            }
        }

        void NextFile()
        {
            SeekFile(1);
        }

        void PreviousFile()
        {
            SeekFile(-1);
        }

        private void tsbPrevious_Click(object sender, EventArgs e)
        {
            if (IsOperationPending)
                Operation_CancelClicked(sender, null);
            PreviousFile();

        }

        private void tsbNext_Click(object sender, EventArgs e)
        {
            if (IsOperationPending)
                Operation_CancelClicked(sender, null);
            NextFile();
        }

        private void frmMain_MouseMove(object sender, MouseEventArgs e)
        {

        }

        void frmMain_MouseWheel(object sender, MouseEventArgs e)
        {
            try
            {
                if (IsOperationPending)
                {
                    if (e.Delta > 0)
                    {
                        imageRenderer1.ZoomIn(10);
                    }
                    else if (e.Delta < 0)
                    {
                        imageRenderer1.ZoomOut(10);
                    }
                }
                else
                {
                    if (e.Delta > 0)
                    {
                        NextFile();
                    }
                    else if (e.Delta < 0)
                    {
                        PreviousFile();
                    }
                }
            }
            catch (Exception)
            {
            }
        }

        void SaveFileAs(string filename)
        {
            string format = System.IO.Path.GetExtension(filename).ToLower();
            switch (format)
            {
                case ".bmp":
                    MyBitmap.Save(filename, System.Drawing.Imaging.ImageFormat.Bmp);
                    break;
                case ".jpg":
                case ".jpeg":
                    MyBitmap.Save(filename, System.Drawing.Imaging.ImageFormat.Jpeg);
                    break;
                case ".png":
                    MyBitmap.Save(filename, System.Drawing.Imaging.ImageFormat.Png);
                    break;
                case ".gif":
                    MyBitmap.Save(filename, System.Drawing.Imaging.ImageFormat.Gif);
                    break;
                default:
                    throw new NotImplementedException("Unknon format : " + format);
            }
        }

        private void tsbSave_Click(object sender, EventArgs e)
        {
            if (MyBitmap != null &&
                MessageBox.Show(this, "Do you want to write over original file?", "Are you sure?", MessageBoxButtons.YesNo, MessageBoxIcon.Exclamation, MessageBoxDefaultButton.Button2)
                == System.Windows.Forms.DialogResult.Yes)
            {
                SaveFileAs(GetFilePath());
            }
        }

        private void tsbSaveAs_Click(object sender, EventArgs e)
        {
            saveFileDialog1.Filter = Path.GetExtension(szRequestedFile) + "|*" + Path.GetExtension(szRequestedFile) + "|BMP|*.bmp|GIF|*.gif|JPG|*.jpg;*.jpeg|PNG|*.png|TIFF|*.tif;*.tiff";
            if (MyBitmap != null &&
                saveFileDialog1.ShowDialog(this) == System.Windows.Forms.DialogResult.OK)
            {
                SaveFileAs(saveFileDialog1.FileName);
            }
        }

        private bool IsOperationPending
        {
            get
            {
                return tableLayoutPanel1.Controls.GetChildIndex(ResizeControl, false) != -1 ||
                     tableLayoutPanel1.Controls.GetChildIndex(CropControl, false) != -1;
            }
        }

        private void tsbResize_Click(object sender, EventArgs e)
        {
            if (!IsOperationPending && MyBitmap != null)
            {
                tableLayoutPanel1.Controls.Add(ResizeControl, 1, 0);
                ResizeControl.SetSize(MyBitmap.Width, MyBitmap.Height);
                OnOperationFinish = delegate
                {
                    this.tableLayoutPanel1.Controls.Remove(ResizeControl);
                };
            }
        }

        private void tsbCrop_Click(object sender, EventArgs e)
        {
            if (!IsOperationPending)
            {
                tableLayoutPanel1.Controls.Add(CropControl, 1, 0);
                Rectangle rect = new Rectangle(MyBitmap.Width / 3, MyBitmap.Height / 3, MyBitmap.Width / 3, MyBitmap.Height / 3);
                imageRenderer1.StartToSelect(rect);
                CropControl.SetRectangle(rect, MyBitmap.Size);

                OnOperationOk = delegate
                {
                    var toDispose = imageRenderer1.SelectedImage;

                    var tmpBitmap = new Bitmap(imageRenderer1.SelectionRect.Width, imageRenderer1.SelectionRect.Height, MyBitmap.PixelFormat);
                    Graphics g = Graphics.FromImage(tmpBitmap);
                    g.DrawImage(MyBitmap, new Rectangle(0, 0, tmpBitmap.Width, tmpBitmap.Height), imageRenderer1.SelectionRect, GraphicsUnit.Pixel);
                    g.Flush();
                    g.Dispose();
                    MyBitmap = tmpBitmap;
                    imageRenderer1.SelectedImage = MyBitmap;
                    IsImageManipulated = true;

                    if (toDispose != MyBitmap)
                        AutoDisposeQueue.Enqueue(toDispose);
                };

                OnOperationFinish = delegate
                {
                    imageRenderer1.EndSelect();
                    this.tableLayoutPanel1.Controls.Remove(CropControl);
                };
            }
        }

        private void tsbRotateLeft_Click(object sender, EventArgs e)
        {
            if (MyBitmap == null)
                return;
            MyBitmap.RotateFlip(RotateFlipType.Rotate270FlipNone);
            imageRenderer1.SelectedImage = MyBitmap;
            IsImageManipulated = true;
        }

        private void tsbRotateRight_Click(object sender, EventArgs e)
        {
            if (MyBitmap == null)
                return;
            MyBitmap.RotateFlip(RotateFlipType.Rotate90FlipNone);
            imageRenderer1.SelectedImage = MyBitmap;
            IsImageManipulated = true;
        }
    }
}
