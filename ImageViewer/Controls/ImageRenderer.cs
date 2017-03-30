using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;
using System.Runtime.InteropServices;
using System.Threading;
using SharpGL;

namespace ImageViewer
{
    public partial class ImageRenderer : OpenGLControl
    {
        Bitmap _Selected = null;

        static int MaximumTextureWidth = 256;
        static int MaximumTextureHeight = 256;

        public double Scale { get; set; }
        public Point Translate { get; set; }

        class Work : IDisposable
        {
            public Bitmap ToLoad;
            public int x, y, w, h;
            public int size;
            public IntPtr ScanPtr;
            public bool Ready;

            public void Dispose()
            {
                // Scanptr should already deleted by Bitmap.UnlockBits
            }
            
            ~Work()
            {
                // No need really
                GC.SuppressFinalize(this);
            }
        }

        Queue<Work> WorkList = new Queue<Work>();
        Queue<Work> DoneWorkList = new Queue<Work>();
        List<Thread> WorkerList = new List<System.Threading.Thread>();
        ManualResetEvent IsLoading = new ManualResetEvent(false);

        bool IsRunnning { get; set; }

        void DoWork()
        {
            Work currentWork = null;
            while (true)
            {
                IsLoading.WaitOne();
                if (!this.IsRunnning)
                    return;
                lock (WorkList)
                {
                    if (WorkList.Count > 0)
                    {
                        currentWork = WorkList.Dequeue();
                    }
                    else
                        currentWork = null;
                }
                if (currentWork == null)
                    Thread.Sleep(1);
                else
                {
                    currentWork.ScanPtr = Marshal.AllocHGlobal(currentWork.size);

                }
            }
        }

        public ImageRenderer()
        {
            this.OpenGLInitialized += ImageRenderer_OpenGLInitialized;
            this.OpenGLDraw += ImageRenderer_OpenGLDraw;
            this.Resized += ImageRenderer_Resized;
        }

        void ImageRenderer_Resized(object sender, EventArgs e)
        {
            if (!this.DesignMode)
            {
                SetupViewport();
                Invalidate();
            }
        }


        void ImageRenderer_OpenGLDraw(object sender, RenderEventArgs args)
        {
            if (this.DesignMode)
                return;

            var gl = this.OpenGL;

            gl.Clear(OpenGL.GL_COLOR_BUFFER_BIT);

            gl.MatrixMode(SharpGL.OpenGL.GL_MODELVIEW);
            gl.LoadIdentity();

            try
            {
                if (TextureList != null)
                {
                    lock (TextureList)
                    {
                        if (TextureList.Count == 0)
                            return;
                        LimitPositions();

                        if (Selecting)
                        {
                            gl.Enable(OpenGL.GL_BLEND);
                            gl.BlendFunc(OpenGL.GL_CONSTANT_ALPHA_EXT, OpenGL.GL_ONE_MINUS_CONSTANT_ALPHA_EXT);
                            gl.BlendColor(1, 1, 1, 0.5f);

                            /*for (int x = -2; x < 3; ++x)
                                for (int y = -2; y < 3; ++y)
                                {
                                    gl.LoadIdentity();
                                    gl.Translate(Translate.X + x, Translate.Y + y, 0);
                                    gl.Scale(Scale, Scale, 1);
                                    DrawImage();
                                }*/

                            gl.LoadIdentity();
                            gl.Translate((double)Translate.X, (double)Translate.Y, 0);
                            gl.Scale(Scale, Scale, 1);
                            DrawImage();

                            gl.BlendColor(1, 1, 1, 1);
                            gl.Disable(OpenGL.GL_BLEND);

                            gl.Enable(OpenGL.GL_SCISSOR_TEST);
                            var scissor = TranslateSelectionToScreen();
                            gl.Scissor(scissor.X, Height - scissor.Y - scissor.Height, scissor.Width, scissor.Height);

                            gl.LoadIdentity();
                            gl.Translate((double)Translate.X, (double)Translate.Y, 0);
                            gl.Scale(Scale, Scale, 1);


                            DrawImage();
                            gl.Disable(OpenGL.GL_SCISSOR_TEST);


                            gl.LoadIdentity();
                            gl.Translate((double)Translate.X, (double)Translate.Y, 0);
                            gl.Scale(Scale, Scale, 1);


                            gl.Enable(OpenGL.GL_BLEND);

                            //gl.BlendFunc(OpenGL.GL_ONE_MINUS_DST_COLOR, OpenGL.GL_ZERO);
                            gl.BlendFunc(OpenGL.GL_SRC_ALPHA, OpenGL.GL_ONE_MINUS_SRC_ALPHA);

                            gl.LoadIdentity();
                            gl.Enable(OpenGL.GL_LINE_STIPPLE);
                            gl.Enable(OpenGL.GL_LINE_SMOOTH);
                            gl.Enable(OpenGL.GL_POLYGON_SMOOTH);
                            gl.Hint(OpenGL.GL_LINE_SMOOTH_HINT, OpenGL.GL_NICEST);
                            gl.Hint(OpenGL.GL_POLYGON_SMOOTH_HINT, OpenGL.GL_NICEST);


                            var scissorF = TranslateSelectionToScreenF();

                            gl.Translate((double)scissorF.X, (double)scissorF.Y, 0);

                            gl.Color(1.0f, 1.0f, 1.0f, 1.0f);
                            gl.LineStipple(1, 0x0F0F);
                            gl.Begin(OpenGL.GL_LINE_LOOP);
                            gl.Vertex(0, 0); gl.Vertex(scissorF.Width, 0); gl.Vertex(scissorF.Width, scissorF.Height); gl.Vertex(0, scissorF.Height);
                            gl.End();

                            gl.Color(0.0f, 0.0f, 0.0f, 1.0f);
                            gl.LineStipple(1, 0xF0F0);
                            gl.Begin(OpenGL.GL_LINE_LOOP);
                            gl.Vertex(0, 0); gl.Vertex(scissorF.Width, 0); gl.Vertex(scissorF.Width, scissorF.Height); gl.Vertex(0, scissorF.Height);
                            gl.End();

                            gl.Disable(OpenGL.GL_LINE_STIPPLE);

                            for (int ix = 0; ix <= 2; ++ix)
                            {
                                for (int iy = 0; iy <= 2; ++iy)
                                {
                                    if (ix == 1 && iy == 1)
                                        continue;
                                    gl.LoadIdentity();
                                    gl.Translate((double)scissorF.X, (double)scissorF.Y, 0);
                                    gl.Translate((scissorF.Width * ix) / 2, (scissorF.Height * iy) / 2, 0);

                                    gl.Color(1.0f, 1.0f, 0.0f, 1.0f);
                                    gl.Begin(OpenGL.GL_QUADS);
                                    gl.Vertex(-4, -4); gl.Vertex(4, -4); gl.Vertex(4, 4); gl.Vertex(-4, 4);
                                    gl.End();

                                    gl.Color(0.0f, 0.0f, 0.0f, 1.0f);
                                    gl.Begin(OpenGL.GL_LINE_LOOP);
                                    gl.Vertex(-4, -4); gl.Vertex(4, -4); gl.Vertex(4, 4); gl.Vertex(-4, 4);
                                    gl.End();
                                }
                            }
                            gl.Disable(OpenGL.GL_BLEND);
                        }
                        else
                        {
                            gl.Translate((double)Translate.X, (double)Translate.Y, 0);
                            gl.Scale(Scale, Scale, 1);
                            DrawImage();
                        }
                    }
                    //gl.Flush();
                }
            }
            catch (Exception) { }
        }

        void ImageRenderer_OpenGLInitialized(object sender, EventArgs e)
        {
            var gl = this.OpenGL;
            gl.Enable(OpenGL.GL_TEXTURE_2D);
            gl.Disable(OpenGL.GL_DEPTH_TEST);
            gl.ClearColor(0, 0, 0, 0);
            IsRunnning = true;
            SetupViewport();
            /*for (int i = 0; i < Environment.ProcessorCount; ++i)
                WorkerList.Add(new Thread(DoWork));*/
            foreach (var worker in WorkerList)
                worker.Start();
        }

        protected override void OnHandleDestroyed(EventArgs e)
        {
            try
            {
                IsRunnning = false;
                IsLoading.Set();
                foreach (var worker in WorkerList)
                    worker.Join();
            }
            catch (Exception)
            {
            }
            finally
            {
                base.OnHandleDestroyed(e);
            }
        }

        struct TextureImage
        {
            public uint Texture;
            public Point Position;
            public Size Size;
        }

        List<TextureImage> TextureList = new List<TextureImage>();

        uint LoadTexture(IntPtr scan0, int w, int h, System.Drawing.Imaging.PixelFormat pixelFormat)
        {
            var gl = this.OpenGL;
            uint id;
            uint[] arr = new uint[1];
            gl.GenTextures(1, arr);
            id = arr[0];
            try
            {
                gl.Enable(OpenGL.GL_TEXTURE_2D);

                gl.BindTexture(OpenGL.GL_TEXTURE_2D, id);

                gl.TexParameter(SharpGL.OpenGL.GL_TEXTURE_2D, SharpGL.OpenGL.GL_TEXTURE_WRAP_S, SharpGL.OpenGL.GL_CLAMP_TO_EDGE);
                gl.TexParameter(SharpGL.OpenGL.GL_TEXTURE_2D, SharpGL.OpenGL.GL_TEXTURE_WRAP_T, SharpGL.OpenGL.GL_CLAMP_TO_EDGE);
                gl.TexParameter(SharpGL.OpenGL.GL_TEXTURE_2D, SharpGL.OpenGL.GL_TEXTURE_MAG_FILTER, SharpGL.OpenGL.GL_LINEAR);
                gl.TexParameter(SharpGL.OpenGL.GL_TEXTURE_2D, SharpGL.OpenGL.GL_TEXTURE_MIN_FILTER, SharpGL.OpenGL.GL_LINEAR);

                uint oPixelFormat = SharpGL.OpenGL.GL_BGR;
                uint pixelInternal = SharpGL.OpenGL.GL_RGB;
                switch (pixelFormat)
                {
                    case System.Drawing.Imaging.PixelFormat.Format24bppRgb:
                        oPixelFormat = SharpGL.OpenGL.GL_BGR;
                        pixelInternal = SharpGL.OpenGL.GL_RGB;
                        break;
                    case System.Drawing.Imaging.PixelFormat.Format32bppArgb:
                    case System.Drawing.Imaging.PixelFormat.Format32bppPArgb:
                    case System.Drawing.Imaging.PixelFormat.Format32bppRgb:
                        oPixelFormat = SharpGL.OpenGL.GL_BGRA;
                        pixelInternal = SharpGL.OpenGL.GL_RGBA;
                        break;
                    default:
                        break;
                }

                gl.TexImage2D(SharpGL.OpenGL.GL_TEXTURE_2D,
                         0,
                         pixelInternal,
                         w,
                         h,
                         0,
                         oPixelFormat,
                         SharpGL.OpenGL.GL_UNSIGNED_BYTE,
                         scan0);

                return id;
            }
            catch (Exception)
            {
                gl.DeleteTextures(1, new uint[] { id });
                throw;
            }
        }

        void ClearTextureList()
        {
            var gl = this.OpenGL;
            while (TextureList.Count > 0)
            {
                uint textureID = TextureList[0].Texture;
                TextureList.RemoveAt(0);
                gl.DeleteTextures(1, new uint[] { textureID });
            }
        }

        void CreateTextureListInner(System.Drawing.Imaging.BitmapData bmp_data)
        {
            int pixelSize = 3;

            switch (bmp_data.PixelFormat)
            {
                case System.Drawing.Imaging.PixelFormat.Format24bppRgb:
                    pixelSize = 3;
                    break;
                case System.Drawing.Imaging.PixelFormat.Format32bppArgb:
                case System.Drawing.Imaging.PixelFormat.Format32bppPArgb:
                case System.Drawing.Imaging.PixelFormat.Format32bppRgb:
                    pixelSize = 4;
                    break;
                default:
                    pixelSize = 4;
                    break;

            }

            int size = MaximumTextureWidth * MaximumTextureHeight * pixelSize;

            IntPtr unmanagedPointer = Marshal.AllocHGlobal(size);
            try
            {
                for (int x = 0; x < _Selected.Width; x += MaximumTextureWidth)
                {
                    for (int y = 0; y < _Selected.Height; y += MaximumTextureHeight)
                    {
                        int w = MaximumTextureWidth;
                        int h = MaximumTextureHeight;

                        if (x + w > _Selected.Width)
                            w = _Selected.Width - x;

                        if (y + h > _Selected.Height)
                            h = _Selected.Height - y;


                        uint scanSize = (uint)(w * pixelSize);

                        NativeMethods.RtlZeroMemory(unmanagedPointer, size);

                        for (int i = 0; i < h; ++i)
                        {
                            NativeMethods.CopyMemory(new IntPtr(unmanagedPointer.ToInt64() + i * MaximumTextureWidth * pixelSize),
                                new IntPtr(bmp_data.Scan0.ToInt64() + x * pixelSize + (i + y) * bmp_data.Stride),
                                scanSize);
                        }

                        uint texture = LoadTexture(unmanagedPointer, MaximumTextureWidth, MaximumTextureHeight, bmp_data.PixelFormat);

                        TextureList.Add(new TextureImage
                        {
                            Texture = texture,
                            Position = new Point(x, y),
                            Size = new Size(MaximumTextureWidth, MaximumTextureHeight)
                        });
                    }
                }
            }
            finally
            {
                Marshal.FreeHGlobal(unmanagedPointer);
            }

        }

        public static System.Drawing.Imaging.BitmapData GetBitmapBits(Bitmap bitmap)
        {
            switch (bitmap.PixelFormat)
            {
                case System.Drawing.Imaging.PixelFormat.Format24bppRgb:
                    return bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height),
                         System.Drawing.Imaging.ImageLockMode.ReadOnly, bitmap.PixelFormat);
                case System.Drawing.Imaging.PixelFormat.Format32bppArgb:
                case System.Drawing.Imaging.PixelFormat.Format32bppPArgb:
                case System.Drawing.Imaging.PixelFormat.Format32bppRgb:
                    return bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height),
                         System.Drawing.Imaging.ImageLockMode.ReadOnly, bitmap.PixelFormat);
                default:
                    return bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height),
                         System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format32bppRgb);
            }
        }

        public static void FreeBitmapBits(Bitmap bitmap, System.Drawing.Imaging.BitmapData bmp_data)
        {
            bitmap.UnlockBits(bmp_data);
        }

        void LimitPositions()
        {
            float x, y;
            x = Translate.X;
            y = Translate.Y;

            if (_Selected.Width * Scale > Width)
            {
                if (x > 0)
                    x = 0;
                if (x < Width - _Selected.Width * Scale)
                    x = (float)(Width - _Selected.Width * Scale);
            }
            else
            {
                x = (float)((Width - _Selected.Width * Scale) / 2);
            }

            if (_Selected.Height * Scale > Height)
            {
                if (y > 0)
                    y = 0;
                if (y < Height - _Selected.Height * Scale)
                    y = (float)(Height - _Selected.Height * Scale);
            }
            else
            {
                y = (float)((Height - _Selected.Height * Scale) / 2);
            }

            Translate = new Point((int)x, (int)y);
        }

        void Zoom(double mul)
        {
            double dx, dy;

            dx = Translate.X - Width / 2.0;
            dy = Translate.Y - Height / 2.0;

            Translate = new Point((int)(Width / 2.0 + dx * mul), (int)(Height / 2.0 + dy * mul));

            this.Scale *= mul;
            Invalidate();
        }

        public void ZoomIn(double percent)
        {
            Zoom(1.0 + percent / 100.0);
        }

        public void ZoomOut(double percent)
        {
            Zoom(1.0 / (1.0 + percent / 100.0));
        }

        public void Move(double dx, double dy)
        {
            Translate = new Point((int)(Translate.X + dx), (int)(Translate.Y + dy));
            Invalidate();
        }

        public void CenterImage()
        {
            Scale = 1;
            Translate = new Point(0, 0);

            if (_Selected.Width > Width)
            {
                Scale = Convert.ToDouble(Width) / _Selected.Width;
            }

            if (_Selected.Height > Height)
            {
                Scale = Math.Min(Scale, Convert.ToDouble(Height) / _Selected.Height);
            }

            Scale = Math.Max(0.001, Scale);
        }

        public Bitmap SelectedImage
        {
            set
            {
                if (!DesignMode)
                {
                    _Selected = value;
                    try
                    {
                        ClearTextureList();
                        var bits = GetBitmapBits(value);
                        try { CreateTextureListInner(bits); }
                        finally { FreeBitmapBits(value, bits); }
                        CenterImage();
                        Invalidate();
                    }
                    catch (Exception) { }
                }
            }
            get
            {
                return _Selected;
            }
        }

        public void SetBitmap(Bitmap bmp, System.Drawing.Imaging.BitmapData bits)
        {
            this._Selected = bmp;
            try
            {
                ClearTextureList();
                CreateTextureListInner(bits);
                CenterImage();
                Invalidate();
            }
            catch (Exception)
            {
            }
        }

        void DrawImage()
        {
            var gl = this.OpenGL;
            gl.Enable(SharpGL.OpenGL.GL_TEXTURE_2D);
            gl.Color(1.0f, 1.0f, 1.0f, 1.0f);
            foreach (var i in TextureList)
            {
                gl.PushMatrix();
                //gl.Translate(i.Position.X, i.Position.Y, 0);
                gl.BindTexture(SharpGL.OpenGL.GL_TEXTURE_2D, i.Texture);

                gl.Begin(SharpGL.OpenGL.GL_QUADS);
                gl.TexCoord(0, 0);
                gl.Vertex(i.Position.X, i.Position.Y);

                gl.TexCoord(1, 0);
                gl.Vertex(i.Size.Width + i.Position.X, i.Position.Y);

                gl.TexCoord(1, 1);
                gl.Vertex(i.Size.Width + i.Position.X, i.Size.Height + i.Position.Y);

                gl.TexCoord(0, 1);
                gl.Vertex(i.Position.X, i.Size.Height + i.Position.Y);
                gl.End();
                gl.PopMatrix();
            }
            gl.Disable(SharpGL.OpenGL.GL_TEXTURE_2D);
        }

        const int SelectionSize = 32;

        public class SelectionUpdateEventArgs: System.EventArgs
        {
            public int X;
            public int Y;
            public int Width;
            public int Height;

            public SelectionUpdateEventArgs(Rectangle rect)
            {
                X = rect.X;
                Y = rect.Y;
                Width = rect.Width;
                Height = rect.Height;
            }

            public Rectangle ToRectangle()
            {
                return new Rectangle(X, Y, Width, Height);
            }
        }

        public event EventHandler<SelectionUpdateEventArgs> SelectionRectangleUpdate;
        bool SelectionRectMoving = false;
        bool SelectionResizing = false;
        Point SelectionResizeMotivation;
        double OldMouseX, OldMouseY;

        Rectangle TranslateSelectionToScreen()
        {
            int translatedRectX = (int)(SelectionRect.X * Scale + Translate.X);
            int translatedRectY = (int)(SelectionRect.Y * Scale + Translate.Y);
            return new Rectangle(translatedRectX, translatedRectY, (int)(SelectionRect.Width * Scale), (int)(SelectionRect.Height * Scale));
        }

        RectangleF TranslateSelectionToScreenF()
        {
            float translatedRectX = (float)(SelectionRect.X * Scale + Translate.X);
            float translatedRectY = (float)(SelectionRect.Y * Scale + Translate.Y);
            return new RectangleF(translatedRectX, translatedRectY, (float)(SelectionRect.Width * Scale), (float)(SelectionRect.Height * Scale));
        }

        bool IsInSelection(Point p)
        {
            var r = TranslateSelectionToScreen();

            if (p.IsInsideRect(new Rectangle(r.X + 3, r.Y + 3, r.Width - 6, r.Height - 6)))
            {
                return true;
            }
            return false;
        }

        bool IsOnSelection(Point p)
        {
            var r = TranslateSelectionToScreen();

            if (p.IsInsideRect(new Rectangle(r.X - 3, r.Y - 3, r.Width + 6, r.Height + 6)) && !p.IsInsideRect(new Rectangle(r.X + 3, r.Y + 3, r.Width - 6, r.Height - 6)))
            {
                return true;
            }
            return false;
        }

        Point GetSelectionMotivation(Point p)
        {
            var r = TranslateSelectionToScreen();

            if (p.IsInsideRect(new Rectangle(r.X - 4, r.Y - 4, 8, 8)))
                return new Point(-1, -1);
            if (p.IsInsideRect(new Rectangle(r.X - 4 + r.Width, r.Y - 4, 8, 8)))
                return new Point(1, -1);
            if (p.IsInsideRect(new Rectangle(r.X - 4 + r.Width, r.Y - 4 + r.Height, 8, 8)))
                return new Point(1, 1);
            if (p.IsInsideRect(new Rectangle(r.X - 4, r.Y - 4 + r.Height, 8, 8)))
                return new Point(-1, 1);

            if (p.IsInsideRect(new Rectangle(r.X - 4, r.Y - 4 + r.Height / 2, 8, 8)))
                return new Point(-1, 0);
            if (p.IsInsideRect(new Rectangle(r.X - 4 + r.Width, r.Y - 4 + r.Height / 2, 8, 8)))
                return new Point(1, 0);
            if (p.IsInsideRect(new Rectangle(r.X - 4 + r.Width / 2, r.Y - 4, 8, 8)))
                return new Point(0, -1);
            if (p.IsInsideRect(new Rectangle(r.X - 4 + r.Width / 2, r.Y - 4 + r.Height, 8, 8)))
                return new Point(0, 1);

            if (p.IsInsideRect(new Rectangle(r.X, r.Y, r.Width - 6, 6)))
                return new Point(0, -1);
            if (p.IsInsideRect(new Rectangle(r.X, r.Y + r.Height, r.Width - 6, 6)))
                return new Point(0, 1);
            if (p.IsInsideRect(new Rectangle(r.X, r.Y, 6, r.Height - 6)))
                return new Point(-1, 0);
            if (p.IsInsideRect(new Rectangle(r.X + r.Width, r.Y, 6, r.Height - 6)))
                return new Point(1, 0);

            return new Point(0, 0);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == System.Windows.Forms.MouseButtons.Left)
            {
                if (Selecting)
                {
                    OldMouseX = e.X / Scale;
                    OldMouseY = e.Y / Scale;

                    var p = GetSelectionMotivation(e.Location);
                    SelectionResizeMotivation = p;
                    if (p.X != 0 || p.Y != 0)
                    {
                        Cursor.Hide();
                        SelectionResizing = true;
                        Invalidate();
                        return;
                    }
                    else if (IsInSelection(e.Location))
                    {
                        Cursor.Hide();
                        SelectionRectMoving = true;
                        Invalidate();
                        return;
                    }
                }
            }
            base.OnMouseDown(e);
        }

        public event EventHandler RunOperationRequested;
        DateTime LastMouseUpTime = DateTime.Now;

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (e.Button == System.Windows.Forms.MouseButtons.Left)
            {

                if (Selecting)
                {
                    var ElapsedMS = (DateTime.Now - LastMouseUpTime).TotalMilliseconds;
                    LastMouseUpTime = DateTime.Now;
                    if (ElapsedMS < 300)
                    {
                        Cursor.Show();
                        SelectionRectMoving = SelectionResizing = false;
                        try
                        {
                            RunOperationRequested.Invoke(this, null);
                        }
                        catch (Exception) { }
                        return;
                    }
                    else
                    {

                        if (SelectionRectMoving)
                        {
                            Cursor.Show();
                            SelectionRectMoving = false;
                            Invalidate();
                            return;
                        }
                        else if (SelectionResizing)
                        {
                            Cursor.Show();
                            SelectionResizing = false;
                            Invalidate();
                            return;
                        }
                    }
                }
            }
            base.OnMouseUp(e);
        }

        public bool MoveSelectionRect(int dx, int dy)
        {
            int x = SelectionRect.X, y = SelectionRect.Y;

            x += dx;
            y += dy;

            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x + SelectionRect.Width > _Selected.Width) x = _Selected.Width - SelectionRect.Width;
            if (y + SelectionRect.Height > _Selected.Height) y = _Selected.Height - SelectionRect.Height;

            if (x != SelectionRect.X || y != SelectionRect.Y)
            {
                SelectionRect = new Rectangle(x, y, SelectionRect.Width, SelectionRect.Height);
                return true;
            }
            return false;
        }


        public bool ResizeSelection(int dx, int dy, Point resizeMotivation)
        {
            var oldSelection = SelectionRect;

            if (dx != 0 || dy != 0)
            {
                OldMouseX += dx;
                OldMouseY += dy;

                int x = SelectionRect.X;
                int y = SelectionRect.Y;
                int w = SelectionRect.Width;
                int h = SelectionRect.Height;

                if (resizeMotivation.X == -1)
                {
                    x += dx;
                    w -= dx;

                    if (w < 1)
                    {
                        x += w - 1;
                        w = 1;
                    }

                    if (x < 0)
                    {
                        w += x;
                        x = 0;
                    }
                }
                else if (resizeMotivation.X == 1)
                {
                    w += dx;

                    if (w < 1)
                    {
                        w = 1;
                    }

                    if (x + w > _Selected.Width)
                    {
                        w = _Selected.Width - x;
                    }
                }

                if (resizeMotivation.Y == -1)
                {
                    y += dy;
                    h -= dy;

                    if (h < 1)
                    {
                        y += h - 1;
                        h = 1;
                    }

                    if (y < 0)
                    {
                        h += y;
                        y = 0;
                    }
                }
                else if (resizeMotivation.Y == 1)
                {
                    h += dy;

                    if (h < 1)
                    {
                        h = 1;
                    }

                    if (y + h > _Selected.Height)
                    {
                        h -= y + h - _Selected.Height;
                    }
                }

                SelectionRect = new Rectangle(x, y, w, h);

                if (oldSelection != SelectionRect)
                    return true;
            }
            return false;
        }

        bool ResizeSelection(int dx, int dy)
        {
            return ResizeSelection(dx, dy, SelectionResizeMotivation);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (Selecting)
            {
                if (SelectionRectMoving)
                {
                    int dx = (int)(e.X / Scale - OldMouseX);
                    int dy = (int)(e.Y / Scale - OldMouseY);

                    if (dx != 0 || dy != 0)
                    {
                        OldMouseX += dx;
                        OldMouseY += dy;

                        if (MoveSelectionRect(dx, dy) && SelectionRectangleUpdate != null)
                            SelectionRectangleUpdate.Invoke(this, new SelectionUpdateEventArgs(SelectionRect));

                        Invalidate();
                    }
                    return;
                }
                else if (SelectionResizing)
                {
                    int dx = (int)(e.X / Scale - OldMouseX);
                    int dy = (int)(e.Y / Scale - OldMouseY);
                    if (ResizeSelection(dx, dy))
                        if (SelectionRectangleUpdate != null)
                            SelectionRectangleUpdate.Invoke(this, new SelectionUpdateEventArgs(SelectionRect));
                    Invalidate();
                }
                else
                {
                    var p = GetSelectionMotivation(e.Location);
                    if (p.X == -1 && p.Y == -1 || p.X == 1 && p.Y == 1)
                        this.Cursor = Cursors.SizeNWSE;
                    else if (p.X == 1 && p.Y == -1 || p.X == -1 && p.Y == 1)
                        this.Cursor = Cursors.SizeNESW;
                    else if (p.X == 1 && p.Y == 0 || p.X == -1 && p.Y == 0)
                        this.Cursor = Cursors.SizeWE;
                    else if (p.X == 0 && p.Y == -1 || p.X == 0 && p.Y == 1)
                        this.Cursor = Cursors.SizeNS;
                    else
                    {
                        if (IsInSelection(e.Location))
                        {
                            this.Cursor = Cursors.SizeAll;
                        }
                        else
                            this.Cursor = Cursors.Arrow;
                    }
                }
            }
            else if (this.Cursor != Cursors.Arrow)
                this.Cursor = Cursors.Arrow;

            base.OnMouseMove(e);
        }

        private void SetupViewport()
        {
            int w = Width;
            int h = Height;
            var gl = this.OpenGL;
            gl.MatrixMode(SharpGL.OpenGL.GL_PROJECTION);
            gl.LoadIdentity();
            gl.Ortho(0, w, h, 0, -1, 1); // Bottom-left corner pixel has coordinate (0, 0)
            gl.Viewport(0, 0, w, h); // Use all of the glControl painting area
            gl.MatrixMode(SharpGL.OpenGL.GL_MODELVIEW);
        }

        protected override bool IsInputKey(Keys keyData)
        {
            switch (keyData)
            {
                case Keys.Right:
                case Keys.Left:
                case Keys.Up:
                case Keys.Down:
                    return true;
                case Keys.Shift | Keys.Right:
                case Keys.Shift | Keys.Left:
                case Keys.Shift | Keys.Up:
                case Keys.Shift | Keys.Down:
                    return true;
            }
            return base.IsInputKey(keyData);
        }

        public Rectangle SelectionRect { get; private set; }
        bool Selecting = false;

        public void StartToSelect(Rectangle SelectionRect)
        {
            this.SelectionRect = SelectionRect;
            Selecting = true;
            Invalidate();
        }

        public bool UpdateSelection(int dx, int dy, Point ResizeMotivation)
        {
            bool CannotMove = true;

            if ((ResizeMotivation.X != 0 && dx != 0) || (ResizeMotivation.Y != 0 && dy != 0))
            {
                if (ResizeSelection(dx, dy, ResizeMotivation))
                    CannotMove = false;
            }

            if (ResizeMotivation.X == 0 && dx != 0)
            {
                if (MoveSelectionRect(dx, 0))
                    CannotMove = false;
            }

            if (ResizeMotivation.Y == 0 && dy != 0)
            {
                if (MoveSelectionRect(0, dy))
                    CannotMove = false;
            }

            if (CannotMove)
                return false;

            Invalidate();
            return true;
        }

        public void EndSelect()
        {
            Selecting = false;
            Invalidate();
        }
    }
}
