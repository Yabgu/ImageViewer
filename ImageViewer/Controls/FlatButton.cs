using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace ImageViewer
{
    public class FlatButton : UserControl
    {
        public FlatButton()
        {

        }

        bool MouseOver = false;

        protected override void OnMouseEnter(EventArgs e)
        {
            MouseOver = true;
            base.OnMouseEnter(e);
            Invalidate();
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            MouseOver = false;
            base.OnMouseLeave(e);
            Invalidate();
        }

        protected override void OnPaintBackground(PaintEventArgs pevent)
        {
            base.OnPaintBackground(pevent);

            var g = pevent.Graphics;
            System.Drawing.Drawing2D.LinearGradientBrush brush;

            brush = new System.Drawing.Drawing2D.LinearGradientBrush(Point.Empty, new Point(0, Height),
                Color.FromArgb(0, 0, 0, 0), Color.FromArgb(70, 0, 0, 0));

            g.FillRectangle(brush, ClientRectangle);
            if (MouseOver)
                g.FillRectangle(new SolidBrush(Color.FromArgb(20, Color.Black)), ClientRectangle);

            g.DrawRectangle(Pens.Black, 0, 0, Width - 1, Height - 1);
        }
    }
}
