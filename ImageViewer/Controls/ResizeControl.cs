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
    public partial class ResizeControl : UserControl
    {
        public ResizeControl()
        {
            InitializeComponent();
            numWidth.ValueChanged += numWidth_ValueChanged;
            numHeight.ValueChanged += numHeight_ValueChanged;
        }

        decimal ratio, oldW = 1, oldH = 1;
        bool Setting = false;

        void numHeight_ValueChanged(object sender, EventArgs e)
        {
            try
            {
                if (!Setting && chkMaintainAspectRatio.Checked)
                {
                    btnDone.Enabled = true;
                    numWidth.Value = numHeight.Value * ratio;
                }
            }
            catch (Exception) { }
        }

        void numWidth_ValueChanged(object sender, EventArgs e)
        {
            try
            {
                if (!Setting && chkMaintainAspectRatio.Checked)
                {
                    btnCancel.Enabled = true;
                    numHeight.Value = numWidth.Value / ratio;
                }
            }
            catch (Exception) { }
        }

        public void SetSize(int width, int height)
        {
            Setting = true;
            oldW = numWidth.Value = width;
            oldH = numHeight.Value = height;
            Setting = false;
            ratio = Convert.ToDecimal(width) / Math.Max(height, 1);
        }

        public class SizeEventArgs : EventArgs
        {
            public int Width;
            public int Height;
            public bool SeamCarving;
        }

        private void timer1_Tick(object sender, EventArgs e)
        {
            try
            {
                if (ValueChanged != null)
                {
                    if (oldW != numWidth.Value || oldH != numHeight.Value)
                    {
                        oldW = numWidth.Value;
                        oldH = numHeight.Value;

                        ValueChanged.Invoke(this, new SizeEventArgs { Width = (int)oldW, Height = (int)oldH, SeamCarving = chkSeamCarving.Checked });
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine(ex.ToString());
            }
        }

        public event EventHandler<SizeEventArgs> ValueChanged;
        public event EventHandler OkClicked;
        public event EventHandler CancelClicked;

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
