using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace ImageViewer
{
    public partial class CropControl : UserControl
    {
        public CropControl()
        {
            InitializeComponent();
            numX.ValueChanged += my_ValueChanged;
            numY.ValueChanged += my_ValueChanged;
            numWidth.ValueChanged += my_ValueChanged;
            numHeight.ValueChanged += my_ValueChanged;
            btnDone.Click += btnDone_Click;
            btnCancel.Click += btnCancel_Click;
        }

        public class CropControlValueChangedArgs : EventArgs
        {
            public int dx { get; set; }
            public int dy { get; set; }
            public Point ResizeMotivation { get; set; }
        }

        public event EventHandler<CropControlValueChangedArgs> ValueChanged;
        public event EventHandler OkClicked;
        public event EventHandler CancelClicked;


        decimal oldX = 0, oldY = 0, oldW = 1, oldH = 1;
        bool Setting = false;

        void my_ValueChanged(object sender, EventArgs e)
        {
            try
            {
                if (!Setting)
                {
                    int dx = 0, dy = 0, mx = 0, my = 0;

                    if (numX.Value != oldX)
                    {
                        dx = Convert.ToInt32(numX.Value - oldX);
                        oldX = numX.Value;
                        mx = 0;
                    }
                    else if (numWidth.Value != oldW)
                    {
                        dx = Convert.ToInt32(numWidth.Value - oldW);
                        oldW = numWidth.Value;
                        mx = 1;
                    }

                    if (numY.Value != oldY)
                    {
                        dy = Convert.ToInt32(numY.Value - oldY);
                        oldY = numY.Value;
                        my = 0;
                    }
                    else if (numHeight.Value != oldH)
                    {
                        dy = Convert.ToInt32(numHeight.Value - oldH);
                        oldH = numHeight.Value;
                        my = 1;
                    }

                    var args = new CropControlValueChangedArgs
                    {
                        dx = dx,
                        dy = dy,
                        ResizeMotivation = new Point(mx, my)
                    };

                    ValueChanged.Invoke(sender, args);
                }
            }
            catch (Exception) { }
        }


        public void SetRectangle(int x, int y, int width, int height, int imageWidth, int imageHeight)
        {
            Setting = true;
            oldX = numX.Value = x;
            oldY = numY.Value = y;
            oldW = numWidth.Value = width;
            oldH = numHeight.Value = height;
            numWidth.Maximum = imageWidth;
            numHeight.Maximum = imageHeight;
            Setting = false;
        }

        public void SetRectangle(Rectangle rect, Size imageSize)
        {
            SetRectangle(rect.X, rect.Y, rect.Width, rect.Height, imageSize.Width, imageSize.Height);
        }


        private void btnDone_Click(object sender, EventArgs e)
        {
            if (OkClicked != null)
            {
                OkClicked.Invoke(this, e);
            }
        }

        private void btnCancel_Click(object sender, EventArgs e)
        {
            if (CancelClicked != null)
            {
                CancelClicked.Invoke(this, e);
            }
        }
    }
}
