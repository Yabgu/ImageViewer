using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace ImageViewer.Effects
{
    public class SeamCarver
    {
        /*    ComputePlatform platform;
            IList<ComputeDevice> devices;
            ComputeContext context;
            ComputeContextPropertyList properties;
            ComputeProgram  program;*/

        public SeamCarver(Bitmap bmp)
        {
            Image = new UInt32[bmp.Width, bmp.Height];

            System.Drawing.Imaging.BitmapData bData = bmp.LockBits(new Rectangle(new Point(), bmp.Size),
                System.Drawing.Imaging.ImageLockMode.ReadOnly,
                System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            try
            {
                int realWidth = bData.Stride / 4;
                int bheight = bmp.Height;
                unsafe
                {
                    UInt32* p = (UInt32*)bData.Scan0.ToPointer();
                    Parallel.For(0, bmp.Width, x =>
                    {
                        for (int y = 0; y < bheight; ++y)
                        {
                            Image[x, y] = p[x + y * realWidth];
                        }
                    });
                }
            }
            finally { bmp.UnlockBits(bData); }


            /*devices = new List<ComputeDevice>();
            platform = ComputePlatform.Platforms[0];
            properties = new ComputeContextPropertyList(platform);
            devices.Add(platform.Devices[0]);
            context = new ComputeContext(devices, properties, null, IntPtr.Zero);
            program = new ComputeProgram(context, clProgramSource);*/
        }

        UInt32[,] Image;

        int Width
        {
            get
            {
                return Image.GetLength(0);
            }
        }

        int Height
        {
            get
            {
                return Image.GetLength(1);
            }
        }

        static int Weight(Color c1, Color c2)
        {
            int dr = c1.R - c2.R;
            int dg = c1.G - c2.G;
            int db = c1.B - c2.B;
            return dr * dr + dg * dg + db * db;
        }

        public Bitmap GetImage()
        {
            Bitmap bmp = new Bitmap(Image.GetLength(0), Image.GetLength(1), System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            System.Drawing.Imaging.BitmapData bData = bmp.LockBits(new Rectangle(new Point(), bmp.Size),
                System.Drawing.Imaging.ImageLockMode.WriteOnly,
                System.Drawing.Imaging.PixelFormat.Format32bppArgb);

            int realWidth = bData.Stride / 4;
            int bheight = bmp.Height;

            unsafe
            {
                UInt32* p = (UInt32*)bData.Scan0.ToPointer();

                Parallel.For(0, bmp.Width, x =>
                {
                    for (int y = 0; y < bheight; ++y)
                    {
                        p[x + y * realWidth] = Image[x, y];
                    }
                });
            }

            // don't forget to unlock the bitmap!!
            bmp.UnlockBits(bData);
            return bmp;
        }

        //Sort-of does a RGB->YUV conversion
        int[,] Grayscale_Image(UInt32[,] Source)
        {
            int[,] result = new int[Source.GetLength(0), Source.GetLength(1)];
            Parallel.For(0, Source.GetLength(0), x =>
            {
                for (int y = 0; y < Source.GetLength(1); y++)
                {
                    UInt32 p1 = Source[x, y];
                    UInt32 r1 = p1 & 0xFF;
                    UInt32 g1 = (p1 & 0xFF00) >> 8;
                    UInt32 b1 = (p1 & 0xFF0000) >> 16;
                    int gray = (int)(299 * r1 + 587 * g1 + 114 * b1);
                    result[x, y] = gray / 1000;
                }
            });
            return result;
        }

        UInt32[,] GrayscaleToColor(Int32[,] Source)
        {
            UInt32[,] res = new UInt32[Source.GetLength(0), Source.GetLength(1)];
            Parallel.For(0, Source.GetLength(0), i =>
            {
                for (int j = 0; j < Source.GetLength(1); ++j)
                {
                    int c = Source[i, j];
                    if (c > 255)
                        c = 255;
                    else if (c < 0)
                        c = 0;
                    res[i, j] = (UInt32)(c | c << 8 | c << 16 | 0xFF000000);
                }
            });
            return res;
        }

        //returns the convolution value of the pixel Source(x,y) with the given kernel
        //EasyBMP Warnings better be off for this, otherwise, lots of errors show up
        int Convolve_Pixel(UInt32[,] Source, int x, int y, int[,] kernel)
        {
            if (kernel.GetLength(1) != 3)
                throw new Exception("Invalid kernel size");

            int convR = 0;
            int convG = 0;
            int convB = 0;

            //these loops are a bit complicted since I need to access pixels all around the called pixel
            for (int xx = 0; xx < 3; xx++)
            {
                for (int yy = 0; yy < 3; yy++)
                {
                    int ix = x + xx - 1;
                    int iy = y + yy - 1;
                    int val = (int)Source[ix, iy];
                    //ix = Math.Max(Math.Min(ix, Source.GetLength(0) - 1), 0);
                    //iy = Math.Max(Math.Min(iy, Source.GetLength(1) - 1), 0);
                    convR += (val & 0xFF) * kernel[xx, yy];
                    convG += ((val & 0xFF00) >> 8) * kernel[xx, yy];
                    convB += ((val & 0xFF0000) >> 16) * kernel[xx, yy];
                }
            }

            return convR * convR + convG * convG + convB * convB;
        }

        int Convolve_Pixel(int[,] Source, int x, int y, int[,] kernel)
        {
            /*  if (kernel.GetLength(1) != 3)
                  throw new Exception("Invalid kernel size");
              */
            int conv = 0;

            //these loops are a bit complicted since I need to access pixels all around the called pixel
            for (int xx = 0; xx < 3; xx++)
            {
                for (int yy = 0; yy < 3; yy++)
                {
                    // int ix = x + xx - 1;
                    //  int iy = y + yy - 1;
                    //ix = Math.Max(Math.Min(ix, Source.GetLength(0) - 1), 0);
                    //iy = Math.Max(Math.Min(iy, Source.GetLength(1) - 1), 0);
                    conv += Source[x + xx - 1, y + yy - 1] * kernel[xx, yy];
                }
            }

            return conv;
        }



        //Prewitt's gradient edge detector kernels, first the x direction, then the y
        readonly int[,] Prewitt_Kernel_X = new int[3, 3]
	    {
		    { 3, 0, -3 },
		    { 10, 0, -10 },
		    { 3, 0, -3 }
	    };

        readonly int[,] Prewitt_Kernel_Y = new int[3, 3]
	    {
		    { 3, 10, 3 },
		    { 0, 0, 0 },
		    { -3, -10, -3 }
	    };

        //performs the Prewitt edge detection on the Source image, places it into Dest
        //Obvious choices for edge detection include Laplacian, Sobel, and Canny.
        //Laplacian doesn't like noise all that much. Sobel can produce great results for rather simple coding.
        //Canny produces perhaps the best edge detection, however, it is very sensitive and requires tweaking to get it better than the rest.
        //I decided to go with a modified Sobel, called Prewitt. It has the ease of coding, but doesn't suffer the isotropic nature of sobel.
        //(ie. it is less particualr to a set of directions around the pixel. Sobel likes the cardinal directions somewhat. They're both decent.)
        //Note: Dest better be of proper size.
        int[,] Edge_Detect(UInt32[,] Source)
        {
            //SetEasyBMPwarningsOff(); //our convolve_pixel() loops _WILL_ try to step out-of-bounds. this is the easy fix
            //easy, as in EasyBMP will constrain us back to the edge pixel
            //There is no easy solution to the edges. Calling the same edge pixel to convolve itself against seems actually better
            //than padding the image with zeros or 255's.
            //Calling itself induces a "ringing" into the near edge of the image. Padding can lead to a darker or lighter edge.
            //The only "good" solution is to have the entire one-pixel wide edge not included in the edge detected image.
            //This would reduce the size of the image by 2 pixels in both directions, something that is unacceptable here.

            int tWidth = Source.GetLength(0);
            int tHeight = Source.GetLength(1);
            int[,] Dest = new int[tWidth, tHeight];



            Parallel.For(1, tWidth - 1, i =>
            {
                for (int j = 1; j < tHeight - 1; ++j)
                {
                    //get the edges for the x and y directions
                    Int32 edge_x = Convolve_Pixel(Source, i, j, Prewitt_Kernel_X);
                    Int32 edge_y = Convolve_Pixel(Source, i, j, Prewitt_Kernel_Y);


                    //add their weights up
                    int edge_value = Math.Abs(edge_x) + Math.Abs(edge_y);

                    Dest[i, j] = edge_value;
                }
            });

            Parallel.For(0, Source.GetLength(0), i =>
            {
                Dest[i, 0] = Dest[i, 1];
                Dest[i, tHeight - 1] = Dest[i, tHeight - 2];
            });

            Parallel.For(0, Source.GetLength(1), j =>
            {
                Dest[0, j] = Dest[1, j];
                Dest[tWidth - 1, j] = Dest[tWidth - 2, j];
            });

            return Dest;
            //SetEasyBMPwarningsOn(); //turn these back on then
        }

        string clProgramSource = @"

constant  int Prewitt_Kernel_X[] = 
{
	3, 0, -3,
	10, 0, -10,
	3, 0, -3
};

constant  int Prewitt_Kernel_Y[] = 
{
	3, 10, 3,
	0, 0, 0,
	-3, -10, -3
};

kernel void Edge_Detect(
    global read_only int* Source,
    global read_only int* Height,
    global write_only int* Dest )
{
    int j, i, h, ih, eval, xx, yy, conv;
    i = get_global_id(0)+1;
    h = Height[0];
    ih = i * h;
    
    for (j = 1; j < h - 1; ++j)
    {
 Dest[ih + j] += ih+j;
        //get the edges for the x and y directions
       /* conv = 0;

        //these loops are a bit complicted since I need to access pixels all around the called pixel
        for (xx = 0; xx != 3; ++xx)
        {
            for (yy = 0; yy != 3; ++yy)
            {
                conv += Source[(i + xx - 1) * h + j + yy - 1] * Prewitt_Kernel_X[xx + yy * 3];
            }
        }

        Dest[ih + j] = abs(conv);

        conv = 0;

        //these loops are a bit complicted since I need to access pixels all around the called pixel
        for (xx = 0; xx < 3; ++xx)
        {
            for (yy = 0; yy < 3; ++yy)
            {
                conv += Source[(i + xx - 1) * h + j + yy - 1]* Prewitt_Kernel_Y[xx + yy * 3];
            }
        }

        Dest[ih + j] += abs(conv);*/
    }
}
";
        Int32[,] Edge_Detect(Int32[,] Source)
        {
            //SetEasyBMPwarningsOff(); //our convolve_pixel() loops _WILL_ try to step out-of-bounds. this is the easy fix
            //easy, as in EasyBMP will constrain us back to the edge pixel
            //There is no easy solution to the edges. Calling the same edge pixel to convolve itself against seems actually better
            //than padding the image with zeros or 255's.
            //Calling itself induces a "ringing" into the near edge of the image. Padding can lead to a darker or lighter edge.
            //The only "good" solution is to have the entire one-pixel wide edge not included in the edge detected image.
            //This would reduce the size of the image by 2 pixels in both directions, something that is unacceptable here.

            int tWidth = Source.GetLength(0);
            int tHeight = Source.GetLength(1);
            Int32[,] Dest = new Int32[tWidth, tHeight];

            /* unsafe
             {
                 fixed (Int32* d = Dest)
                 {
                     fixed (Int32* s = Source)
                     {
                         ComputeBuffer<Int32> S = new ComputeBuffer<Int32>(context, ComputeMemoryFlags.ReadOnly | ComputeMemoryFlags.CopyHostPointer, tWidth * tHeight, (IntPtr)s);
                         ComputeBuffer<Int32> W = new ComputeBuffer<Int32>(context, ComputeMemoryFlags.ReadOnly | ComputeMemoryFlags.CopyHostPointer, new Int32[] { tHeight });
                         ComputeBuffer<Int32> D = new ComputeBuffer<Int32>(context, ComputeMemoryFlags.WriteOnly, tWidth * tWidth);
                         try
                         {
                             program.Build(null, null, null, IntPtr.Zero);


                             // Create the kernel function and set its arguments.
                             ComputeKernel kernel = program.CreateKernel("Edge_Detect");
                             kernel.SetMemoryArgument(0, S);
                             kernel.SetMemoryArgument(1, W);
                             kernel.SetMemoryArgument(2, D);

                             // Create the event wait list. An event list is not really needed for this example but it is important to see how it works.
                             // Note that events (like everything else) consume OpenCL resources and creating a lot of them may slow down execution.
                             // For this reason their use should be avoided if possible.
                             ComputeEventList eventList = new ComputeEventList();

                             // Create the command queue. This is used to control kernel execution and manage read/write/copy operations.
                             ComputeCommandQueue commands = new ComputeCommandQueue(context, context.Devices[0], ComputeCommandQueueFlags.None);

                             // Execute the kernel "count" times. After this call returns, "eventList" will contain an event associated with this command.
                             // If eventList == null or typeof(eventList) == ReadOnlyCollection<ComputeEventBase>, a new event will not be created.
                             commands.Execute(kernel, null, new long[] { tWidth-2 }, null, eventList);


                             // Read back the results. If the command-queue has out-of-order execution enabled (default is off), ReadFromBuffer 
                             // will not execute until any previous events in eventList (in our case only eventList[0]) are marked as complete 
                             // by OpenCL. By default the command-queue will execute the commands in the same order as they are issued from the host.
                             // eventList will contain two events after this method returns.
                             commands.Read<Int32>(D, false, 0, (uint)(tWidth * tHeight), (IntPtr)d, eventList);

                             // A blocking "ReadFromBuffer" (if 3rd argument is true) will wait for itself and any previous commands
                             // in the command queue or eventList to finish execution. Otherwise an explicit wait for all the opencl commands 
                             // to finish has to be issued before "arrC" can be used. 
                             // This explicit synchronization can be achieved in two ways:

                             // 1) Wait for the events in the list to finish,
                             //eventList.Wait();

                             // 2) Or simply use
                             commands.Finish();
                         }
                         finally
                         {
                             D.Dispose();
                             S.Dispose();
                             W.Dispose();
                         }

                     }
                 }
             }*/

            Parallel.For(1, tWidth - 1, i =>
            {
                for (int j = 1; j < tHeight - 1; ++j)
                {
                    //get the edges for the x and y directions
                    int edge_x = Convolve_Pixel(Source, i, j, Prewitt_Kernel_X);
                    int edge_y = Convolve_Pixel(Source, i, j, Prewitt_Kernel_Y);


                    //add their weights up
                    int edge_value = Math.Abs(edge_x) + Math.Abs(edge_y);

                    Dest[i, j] = edge_value;
                }
            });

            Parallel.For(0, Source.GetLength(0), i =>
            {
                Dest[i, 0] = Dest[i, 1];
                Dest[i, tHeight - 1] = Dest[i, tHeight - 2];
            });

            Parallel.For(0, Source.GetLength(1), j =>
            {
                Dest[0, j] = Dest[1, j];
                Dest[tWidth - 1, j] = Dest[tWidth - 2, j];
            });

            return Dest;
            //SetEasyBMPwarningsOn(); //turn these back on then
        }

        //Constrains in the X direction
        //If infinite_limit == true, the maximum integer value will be returned if [x][y] is out-of-bounds.
        //If false, it will return 0 if out-of-bounds. Otherwise, it will return the actual value.
        int Get_Element(int[,] Source, int x, int y, bool infinite_limit)
        {
            if (infinite_limit == true)
            {
                if ((x < 0) || (x >= Source.GetLength(0)))
                {
                    return int.MaxValue;
                }
            }
            else
            {
                if ((x < 0) || (x >= Source.GetLength(0)))
                {
                    return 0;
                }
            }

            return Source[x, y];
        }

        //Simple fuction returning the minimum of three values.
        int min_of_three(int x, int y, int z)
        {
            int min = y;

            if (x < min)
            {
                min = x;
            }
            if (z < min)
            {
                min = z;
            }

            return min;
        }

        //This calculates a minimum energy path from the given start point (min_x) and the energy map.
        //Note: Path better be of proper size.
        void Generate_Path(int[,] Energy, int min_x, ref Point[] Path)
        {
            Point min;
            int x = min_x;
            for (int y = Energy.GetLength(1) - 1; y >= 0; y--) //builds from bottom up
            {
                min = new Point(x, y);

                if (Get_Element(Energy, x - 1, y, true) < Get_Element(Energy, min.X, min.Y, true)) //check to see if min is up-left
                {
                    min = new Point(x - 1, y);
                }
                if (Get_Element(Energy, x + 1, y, true) < Get_Element(Energy, min.X, min.Y, true)) //up-right
                {
                    min = new Point(x + 1, y);
                }

                Path[y] = min;
                x = min.X;
            }
        }


        //Energy_Path() generates the least energy Path of the Edge and Weights.
        //This uses a dynamic programming method to easily calculate the path and energy map (see wikipedia for a good example).
        //Weights should be of the same size as Edge, Path should be of proper length (the height of Edge).
        void Energy_Path(int[,] Edge, int[,] Weights, ref Point[] Path)
        {

            int min_x = 0;
            int[,] Energy_Map = new int[Edge.GetLength(0), Edge.GetLength(1)];
            int w = Edge.GetLength(0);
            int h = Edge.GetLength(1);

            //set the first row with the correct energy
            if (Weights != null)
            {
                Parallel.For(0, Edge.GetLength(0), x =>
                {
                    Energy_Map[x, 0] = Edge[x, 0] + Weights[x, 0];
                });

                Parallel.For(1, Edge.GetLength(1), y => //start just after the first correct row
                {
                    for (int x = 0; x < Edge.GetLength(0); x++)
                    {
                        //grab the minimum of straight up, up left, or up right
                        int min = min_of_three(Get_Element(Energy_Map, x - 1, y - 1, true),
                                             Get_Element(Energy_Map, x, y - 1, true),
                                             Get_Element(Energy_Map, x + 1, y - 1, true));
                        //set the energy of the pixel
                        Energy_Map[x, y] = min + Edge[x, y] + Weights[x, y];
                    }
                });
            }
            else
            {
                Parallel.For(0, h - 1, y => //start just after the first correct row
                {
                    for (int x = 0; x < w; x++)
                    {
                        //grab the minimum of straight up, up left, or up right
                        int min = min_of_three(Energy_Map[Math.Max(x - 1, 0), y],
                            Energy_Map[x, y],
                            Energy_Map[Math.Min(x + 1, 0), y]);

                        //set the energy of the pixel
                        Energy_Map[x, y + 1] = min + Edge[x, y + 1];
                    }
                });
            }

            //find minimum path start
            for (int x = 0; x < Energy_Map.GetLength(0); x++)
            {
                if (Energy_Map[x, Energy_Map.GetLength(1) - 1] < Energy_Map[min_x, Energy_Map.GetLength(1) - 1])
                {
                    min_x = x;
                }
            }

            //generate the path back from the energy map
            Generate_Path(Energy_Map, min_x, ref Path);
        }

        //averages two pixels and returns the values
        UInt32 Average_Pixels(UInt32 p1, UInt32 p2)
        {
            UInt32 r1 = p1 & 0xFF, r2 = p2 & 0xFF;
            UInt32 g1 = (p1 & 0xFF00) >> 8, g2 = (p2 & 0xFF00) >> 8;
            UInt32 b1 = (p1 & 0xFF0000) >> 16, b2 = (p2 & 0xFF0000) >> 16;
            UInt32 a1 = (p1 & 0xFF000000) >> 24, a2 = (p2 & 0xFF000000) >> 24;

            return ((r1 + r2) >> 1) | (((g1 + g2) >> 1) << 8) | (((b1 + b2) >> 1) << 16) | (((a1 + a2) >> 1) << 24);
        }

        //Removes the requested path from the Edge, Weights, and the image itself.
        //Edge and the image have the path blended back into the them.
        //Weights and Edge better match the dimentions of Source! Path needs to be the same length as the height of the image!
        UInt32[,] Remove_Path(UInt32[,] Source, Point[] Path, int[,] Edge)
        {
            //SetEasyBMPwarningsOff();

            //unfortunetly a resize erases the image, so I have to refill even the unchanged portions.

            int w = Source.GetLength(0);
            int h = Source.GetLength(1);

            UInt32[,] Dest = new UInt32[w - 1, h];
            Parallel.For(0, h, y =>
            {
                //reduce each row by one, the removed pixel
                var remove = Path[y];
                for (int x = 0; x < Source.GetLength(0) - 1; x++)
                {
                    if (x < remove.X)
                    {
                        Dest[x, y] = Source[x, y];
                    }
                    else if (x > remove.X)
                    {
                        //shift over the pixels
                        Dest[x, y] = Source[x + 1, y];
                        //Weights[x, y] = Get_Element(Weights, x + 1, y, false);
                        //Edge[x, y] = Get_Element(Edge, x + 1, y, false); //always being careful not to step out-of-bounds
                    }
                    else
                    {
                        //average removed pixel back in
                        if (x != 0)
                            Dest[x - 1, y] = Average_Pixels(Source[x, y], Source[x - 1, y]);
                        if (x >= Source.GetLength(0) - 1)
                            Dest[x, y] = Source[x, y];
                        else
                            Dest[x, y] = Average_Pixels(Source[x, y], Source[x + 1, y]);

                    }
                }
            });
  
            return Dest;
        }

        //Removes all requested vertical paths form the image.
        UInt32[,] CAIR_Remove(UInt32[,] Source, int[,] Weights, int goal_x)
        {
            UInt32[,] Dest = Source;
            int[,] Grayscale;
            int removes = Source.GetLength(0) - goal_x;
            int[,] Edge = new int[Source.GetLength(0), Source.GetLength(1)];
            Point[] Min_Path = new Point[Source.GetLength(1)];

            //we must do this the first time. quality will determine how often we do this once we get going
            //Temp = Transfer_Image(Source);
            Grayscale = Grayscale_Image(Source);
            Edge = Edge_Detect(Grayscale);

            for (int i = 0; i < removes; i++)
            {
                Energy_Path(Edge, Weights, ref Min_Path);
                Dest = Remove_Path(Dest, Min_Path, Edge);
                //Temp = Transfer_Image(Dest);

                //quality represents a precent of how often during the remove cycle we recalculate the edge from the image
                //so, I do this little wonder here to accomplish that
                /* if (((i % (int)(1 / quality)) == 0) && (quality != 0))
                 {
                     Grayscale = Grayscale_Image(Dest);
                     Edge = Edge_Detect(Grayscale); //don't need to resize since Remove_Path already did
                 }*/
            }

            return Dest;
        }

        //The Great CAIR Frontend. This baby will resize Source using Weights into the dimensions supplied by goal_x and goal_y into Dest.
        //It will use quality as a factor to determine how often the edge is generated between removals.
        //Source can be of any size. goal_x and goal_y must be less than the dimensions of the Source image for anything to happen.
        //Weights allows for an area to be biased for remvoal/protection. A large positive value will protect a portion of the image,
        //and a large negative value will remove it. Do not exceed the limits of int's, as this will cause an overflow. I would suggest
        //a safe range of -2,000,000 to 2,000,000.
        //Weights must be the same size as Source. It will be scaled down with Source as paths are removed. Dest is the output,
        //and as such has no constraints (its contents will be destroyed, just so you know). Quality should be >0 and <=1. A >1 quality
        //will work just like 1, meaning the edge will be recalculated after every removal (and a <0 quality will also work like 1).
        UInt32[,] CAIR(UInt32[,] Source, int goal_x, int goal_y, double quality)
        {
            UInt32[,] Dest = Source;
            Int32[,] Weights = null; //Edge_Detect(Source);

            if (goal_x < Source.GetLength(0))
            {
                Dest = CAIR_Remove(Source, Weights, goal_x);
            }

            if (goal_y < Source.GetLength(1))
            {
                //remove horiztonal paths
                //works like above, except hand it a rotated image AND weights
                UInt32[,] TSource;
                UInt32[,] TDest;
                int[,] TWeights = null;
                TSource = Dest.TransposeRowsAndColumns();
                if (Weights != null)
                    TWeights = Weights.TransposeRowsAndColumns();
                TDest = CAIR_Remove(TSource, TWeights, goal_y);
                //store back the transposed info
                Dest = TDest.TransposeRowsAndColumns();
            }

            return Dest;
        }

        public void RemoveVerticalSeam()
        {
            Image = CAIR(Image, Image.GetLength(0) - 1, Image.GetLength(1), 1);
        }

        public void RemoveHorizontalSeam()
        {
            Image = CAIR(Image, Image.GetLength(0), Image.GetLength(1) - 1, 1);
        }

        public void AddVerticalSeam()
        {
            Image = CAIR(Image, Image.GetLength(0) + 1, Image.GetLength(1), 1);
        }

        public void AddHorizontalSeam()
        {
            Image = CAIR(Image, Image.GetLength(0), Image.GetLength(1) + 1, 1);
        }
    }
}
