namespace ImageViewer
{
    partial class CropControl
    {
        /// <summary> 
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary> 
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Component Designer generated code

        /// <summary> 
        /// Required method for Designer support - do not modify 
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.label3 = new System.Windows.Forms.Label();
            this.numHeight = new ImageViewer.FlatNumericUpDown();
            this.numWidth = new ImageViewer.FlatNumericUpDown();
            this.label1 = new System.Windows.Forms.Label();
            this.btnCancel = new System.Windows.Forms.Button();
            this.btnDone = new System.Windows.Forms.Button();
            this.label2 = new System.Windows.Forms.Label();
            this.numX = new ImageViewer.FlatNumericUpDown();
            this.numY = new ImageViewer.FlatNumericUpDown();
            ((System.ComponentModel.ISupportInitialize)(this.numHeight)).BeginInit();
            ((System.ComponentModel.ISupportInitialize)(this.numWidth)).BeginInit();
            ((System.ComponentModel.ISupportInitialize)(this.numX)).BeginInit();
            ((System.ComponentModel.ISupportInitialize)(this.numY)).BeginInit();
            this.SuspendLayout();
            // 
            // label3
            // 
            this.label3.AutoSize = true;
            this.label3.Font = new System.Drawing.Font("Microsoft Sans Serif", 11.25F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point, ((byte)(162)));
            this.label3.Location = new System.Drawing.Point(3, 0);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(110, 18);
            this.label3.TabIndex = 10;
            this.label3.Text = "Crop Operation";
            // 
            // numHeight
            // 
            this.numHeight.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.numHeight.Location = new System.Drawing.Point(109, 55);
            this.numHeight.Maximum = new decimal(new int[] {
            1215752191,
            23,
            0,
            0});
            this.numHeight.Minimum = new decimal(new int[] {
            1,
            0,
            0,
            0});
            this.numHeight.Name = "numHeight";
            this.numHeight.Size = new System.Drawing.Size(66, 21);
            this.numHeight.TabIndex = 8;
            this.numHeight.TextAlign = System.Windows.Forms.HorizontalAlignment.Right;
            this.numHeight.Value = new decimal(new int[] {
            1,
            0,
            0,
            0});
            // 
            // numWidth
            // 
            this.numWidth.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.numWidth.Location = new System.Drawing.Point(37, 55);
            this.numWidth.Maximum = new decimal(new int[] {
            1215752191,
            23,
            0,
            0});
            this.numWidth.Minimum = new decimal(new int[] {
            1,
            0,
            0,
            0});
            this.numWidth.Name = "numWidth";
            this.numWidth.Size = new System.Drawing.Size(66, 21);
            this.numWidth.TabIndex = 9;
            this.numWidth.TextAlign = System.Windows.Forms.HorizontalAlignment.Right;
            this.numWidth.Value = new decimal(new int[] {
            1,
            0,
            0,
            0});
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Location = new System.Drawing.Point(4, 57);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(26, 13);
            this.label1.TabIndex = 7;
            this.label1.Text = "Size";
            // 
            // btnCancel
            // 
            this.btnCancel.BackgroundImage = global::ImageViewer.Properties.Resources.bg;
            this.btnCancel.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
            this.btnCancel.Location = new System.Drawing.Point(101, 81);
            this.btnCancel.Name = "btnCancel";
            this.btnCancel.Size = new System.Drawing.Size(74, 23);
            this.btnCancel.TabIndex = 11;
            this.btnCancel.Text = "Cancel";
            this.btnCancel.UseVisualStyleBackColor = true;
            // 
            // btnDone
            // 
            this.btnDone.BackgroundImage = global::ImageViewer.Properties.Resources.bg;
            this.btnDone.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
            this.btnDone.Location = new System.Drawing.Point(21, 81);
            this.btnDone.Name = "btnDone";
            this.btnDone.Size = new System.Drawing.Size(74, 23);
            this.btnDone.TabIndex = 12;
            this.btnDone.Text = "Done";
            this.btnDone.UseVisualStyleBackColor = true;
            // 
            // label2
            // 
            this.label2.AutoSize = true;
            this.label2.Location = new System.Drawing.Point(4, 31);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(24, 13);
            this.label2.TabIndex = 7;
            this.label2.Text = "Pos";
            // 
            // numX
            // 
            this.numX.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.numX.Location = new System.Drawing.Point(37, 29);
            this.numX.Maximum = new decimal(new int[] {
            1215752191,
            23,
            0,
            0});
            this.numX.Name = "numX";
            this.numX.Size = new System.Drawing.Size(66, 21);
            this.numX.TabIndex = 9;
            this.numX.TextAlign = System.Windows.Forms.HorizontalAlignment.Right;
            // 
            // numY
            // 
            this.numY.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.numY.Location = new System.Drawing.Point(109, 29);
            this.numY.Maximum = new decimal(new int[] {
            1215752191,
            23,
            0,
            0});
            this.numY.Name = "numY";
            this.numY.Size = new System.Drawing.Size(66, 21);
            this.numY.TabIndex = 8;
            this.numY.TextAlign = System.Windows.Forms.HorizontalAlignment.Right;
            // 
            // CropControl
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.BackgroundImage = global::ImageViewer.Properties.Resources.bgbody;
            this.Controls.Add(this.btnCancel);
            this.Controls.Add(this.btnDone);
            this.Controls.Add(this.label3);
            this.Controls.Add(this.numY);
            this.Controls.Add(this.numX);
            this.Controls.Add(this.numHeight);
            this.Controls.Add(this.label2);
            this.Controls.Add(this.numWidth);
            this.Controls.Add(this.label1);
            this.Font = new System.Drawing.Font("Tahoma", 8.25F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point, ((byte)(162)));
            this.Name = "CropControl";
            this.Size = new System.Drawing.Size(183, 115);
            ((System.ComponentModel.ISupportInitialize)(this.numHeight)).EndInit();
            ((System.ComponentModel.ISupportInitialize)(this.numWidth)).EndInit();
            ((System.ComponentModel.ISupportInitialize)(this.numX)).EndInit();
            ((System.ComponentModel.ISupportInitialize)(this.numY)).EndInit();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.Label label3;
        private ImageViewer.FlatNumericUpDown numHeight;
        private ImageViewer.FlatNumericUpDown numWidth;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Button btnCancel;
        private System.Windows.Forms.Button btnDone;
        private System.Windows.Forms.Label label2;
        private ImageViewer.FlatNumericUpDown numX;
        private ImageViewer.FlatNumericUpDown numY;
    }
}
