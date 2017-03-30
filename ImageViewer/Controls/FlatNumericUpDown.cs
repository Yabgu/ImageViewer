using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace ImageViewer
{
    /// <summary>
    /// This class does what NumericUpDown control was supposed to do - make up and down keys flat if the
    /// control's border style set to FixedSingle. Its implementation relies heavily on the NumericUpDown's
    /// internal structure.
    /// </summary>
    public class FlatNumericUpDown : NumericUpDown
    {
        FieldInfo buttonState;

        public static void SetStyleHack(Control control, ControlStyles flag)
        {
            control.GetType().GetMethod("SetStyle", BindingFlags.NonPublic | BindingFlags.Instance).Invoke(control, new object[] { flag, true });
        }

        private Control GetUpDownButtons()
        {
            return Controls[0];
        }

        public FlatNumericUpDown()
            : base()
        {
            GetUpDownButtons().Paint += new PaintEventHandler(upDownButtons_Paint);
            buttonState = GetUpDownButtons().GetType().GetField("pushed", BindingFlags.NonPublic | BindingFlags.Instance);
            SetStyleHack(GetUpDownButtons(), ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.DoubleBuffer);
        }


        protected bool DownButtonPushed()
        {
            return ((int)buttonState.GetValue(GetUpDownButtons())) == 2;
        }

        protected bool UpButtonPushed()
        {
            return ((int)buttonState.GetValue(GetUpDownButtons())) == 1;
        }

        private void upDownButtons_Paint(object sender, PaintEventArgs e)
        {
            int height = (ClientSize.Height - 2) / 2;
            ButtonState flatState = BorderStyle == BorderStyle.FixedSingle ? ButtonState.Flat : ButtonState.Normal;

            ControlPaint.DrawScrollButton(e.Graphics, new Rectangle(0, 0, 16, height), ScrollButton.Up,
               flatState | (UpButtonPushed() ? ButtonState.Pushed : 0));
            ControlPaint.DrawScrollButton(e.Graphics, new Rectangle(0, height + 1, 16, height), ScrollButton.Down,
               flatState | (DownButtonPushed() ? ButtonState.Pushed : 0));
        }
    }
}
