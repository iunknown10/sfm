/*
# Copyright (c) 2014-2015, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <string>
#include <memory>
#include <NVX/nvx.h>
#include <NVX/nvx_timer.hpp>
#include <NVX/sfm/sfm.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include "NVXIO/Application.hpp"
#include "NVXIO/FrameSource.hpp"
#include "NVXIO/Utility.hpp"
#include <vector>
#include "SfM.hpp"
#include "utils.hpp"
#include "NVXIO/Render3D.hpp"
#include "MarkerRecognizer.h"
//
// main - Application entry point
//
using namespace cv;
using namespace std;
int VX_to_CV_Image(Mat** mat, vx_image image)
{
    vx_status status = VX_SUCCESS;
    vx_uint32 width = 0; vx_uint32 height = 0; vx_df_image format = VX_DF_IMAGE_VIRT; int CV_format = 0; vx_size planes = 0;

    vxQueryImage(image, VX_IMAGE_ATTRIBUTE_WIDTH, &width, sizeof(width));
    vxQueryImage(image, VX_IMAGE_ATTRIBUTE_HEIGHT, &height, sizeof(height));
    vxQueryImage(image, VX_IMAGE_ATTRIBUTE_FORMAT, &format, sizeof(format));
    vxQueryImage(image, VX_IMAGE_ATTRIBUTE_PLANES, &planes, sizeof(planes));

    if (format == VX_DF_IMAGE_U8)CV_format = CV_8U;
    if (format == VX_DF_IMAGE_S16)CV_format = CV_16S;
    if (format == VX_DF_IMAGE_RGBX)CV_format = CV_8UC4;

    if (format != VX_DF_IMAGE_U8 && format != VX_DF_IMAGE_S16 && format != VX_DF_IMAGE_RGBX)
    {
        vxAddLogEntry((vx_reference)image, VX_ERROR_INVALID_FORMAT, "VX_to_CV_Image ERROR: Image type not Supported in this RELEASE\n"); return VX_ERROR_INVALID_FORMAT;
    }

    Mat * m_cv;	m_cv = new Mat(height, width, CV_format); Mat *pMat = (Mat *)m_cv;
    vx_rectangle_t rect; rect.start_x = 0; rect.start_y = 0; rect.end_x = width; rect.end_y = height;

    vx_uint8 *src[4] = { NULL, NULL, NULL, NULL }; vx_uint32 p; void *ptr = NULL;
    vx_imagepatch_addressing_t addr[4] = { 0, 0, 0, 0 }; vx_uint32 y = 0u;

    for (p = 0u; (p < (int)planes); p++)
    {
        vxAccessImagePatch(image, &rect, p, &addr[p], (void **)&src[p], VX_READ_ONLY);
        size_t len = addr[p].stride_x * (addr[p].dim_x * addr[p].scale_x) / VX_SCALE_UNITY;
        for (y = 0; y < height; y += addr[p].step_y)
        {
            ptr = vxFormatImagePatchAddress2d(src[p], 0, y - rect.start_y, &addr[p]);
            memcpy(pMat->data + y * pMat->step, ptr, len);
        }
    }

    for (p = 0u; p < (int)planes; p++)
        vxCommitImagePatch(image, &rect, p, &addr[p], src[p]);

    *mat = pMat;

    return status;
}


int main(int argc, char* argv[])
{
//   try
    {
        nvxio::Application &app = nvxio::Application::get();

        //
        // Parse command line arguments
        //

        std::string sourceUri = app.findSampleFilePath("sfm/parking_sfm.mp4");
        std::string configFile = app.findSampleFilePath("sfm/sfm_config.ini");
        bool fullPipeline = false;
        std::string maskFile;
        bool noLoop = false;

        app.setDescription("This sample demonstrates Structure from Motion (SfM) algorithm");
        app.addOption(0, "mask", "Optional mask", nvxio::OptionHandler::string(&maskFile));
        app.addBooleanOption('f', "fullPipeline", "Run full SfM pipeline without using IMU data", &fullPipeline);
        app.addBooleanOption('n', "noLoop", "Run sample without loop", &noLoop);

        app.init(argc, argv);

        nvx_module_version_t sfmVersion;
        nvxSfmGetVersion(&sfmVersion);
        std::cout << "VisionWorks SFM version: " << sfmVersion.major << "." << sfmVersion.minor
                  << "." << sfmVersion.patch << sfmVersion.suffix << std::endl;

        std::string imuDataFile;
        std::string frameDataFile;
        if (!fullPipeline)
        {
            imuDataFile = app.findSampleFilePath("sfm/imu_data.txt");
            frameDataFile = app.findSampleFilePath("sfm/images_timestamps.txt");
        }

        if (app.getPreferredRenderName() != "default")
        {
            std::cerr << "The sample uses custom Render for GUI. --nvxio_render option is not supported!" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RENDER;
        }

        //
        // Read SfMParams
        //

        nvx::SfM::SfMParams params;

        std::string msg;
        if (!read(configFile, params, msg))
        {
            std::cout << msg << std::endl;
            return nvxio::Application::APP_EXIT_CODE_INVALID_VALUE;
        }

        //
        // Create OpenVX context
        //

        nvxio::ContextGuard context;

        //
        // Messages generated by the OpenVX framework will be processed by nvxio::stdoutLogCallback
        //

        vxRegisterLogCallback(context, &nvxio::stdoutLogCallback, vx_false_e);

        //
        // Add SfM kernels
        //


        NVXIO_SAFE_CALL(nvxSfmRegisterKernels(context));

        //
        // Create a Frame Source
        //

        std::unique_ptr<nvxio::FrameSource> source(
             nvxio::createDefaultFrameSource(context, sourceUri));

        if (!source || !source->open())
        {
            std::cout << "Can't open source file: " << sourceUri << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RESOURCE;
        }

        nvxio::FrameSource::Parameters sourceParams = source->getConfiguration();

        //
        // Create OpenVX Image to hold frames from video source
        //

        vx_image frame = vxCreateImage(context,
                                       sourceParams.frameWidth, sourceParams.frameHeight, sourceParams.format);
        NVXIO_CHECK_REFERENCE(frame);

        vx_image frame1 = vxCreateImage(context,
                                       sourceParams.frameWidth, sourceParams.frameHeight, sourceParams.format);
        NVXIO_CHECK_REFERENCE(frame1);


        //
        // Load mask image if needed
        //

        vx_image mask = NULL;
        if (!maskFile.empty())
        {
            mask = nvxio::loadImageFromFile(context, maskFile, VX_DF_IMAGE_U8);

            vx_uint32 mask_width = 0, mask_height = 0;
            vxQueryImage(mask, VX_IMAGE_ATTRIBUTE_WIDTH, &mask_width, sizeof(mask_width));
            vxQueryImage(mask, VX_IMAGE_ATTRIBUTE_HEIGHT, &mask_height, sizeof(mask_height));

            if (mask_width != sourceParams.frameWidth || mask_height != sourceParams.frameHeight)
            {
                std::cerr << "The mask must have the same size as the input source." << std::endl;
                return nvxio::Application::APP_EXIT_CODE_INVALID_DIMENSIONS;
            }
        }

        //
        //Creat vision position estimator
        //
        MarkerRecognizer m_recognizer;
        Mat coorMatrix = Mat::zeros(4,4,CV_64F);

        //
        // Create 3D Render instance
        //
        std::unique_ptr<nvxio::Render3D> render3D(nvxio::createDefaultRender3D(context, 0, 0,
            "SfM Point Cloud", sourceParams.frameWidth, sourceParams.frameHeight));

        nvxio::Render::TextBoxStyle style = {{255, 255, 255, 255}, {0, 0, 0, 255}, {10, 10}};

        if (!render3D)
        {
            std::cerr << "Can't create a renderer" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RENDER;
        }

        float fovYinRad = 2.f * atanf(sourceParams.frameHeight / 2.f / params.pFy);
        render3D->setDefaultFOV(180.f / nvxio::PI_F * fovYinRad);

        EventData eventData;
        render3D->setOnKeyboardEventCallback(eventCallback, &eventData);

        //
        // Create SfM class instance
        //

        std::unique_ptr<nvx::SfM> sfm(nvx::SfM::createSfM(context, params));

        //
        // Create FenceDetectorWithKF class instance
        //
        FenceDetectorWithKF fenceDetector;


        nvxio::FrameSource::FrameStatus frameStatus;
        do
        {
            frameStatus = source->fetch(frame);
        }
        while (frameStatus == nvxio::FrameSource::TIMEOUT);

        if (frameStatus == nvxio::FrameSource::CLOSED)
        {
            std::cerr << "Source has no frames" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_FRAMESOURCE;
        }

        vx_status status = sfm->init(frame, mask, imuDataFile, frameDataFile);
        if (status != VX_SUCCESS)
        {
            std::cerr << "Failed to initialize the algorithm" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_ERROR;
        }//                cv::Mat fran=cv::Mat::eye(100,100,CV_8UC3);
        //                cv::gpu::GpuMat frames,frame1;
        //                frames.upload(fran);
        //                cv::gpu::cvtColor(frames,frame1,CV_BGR2GRAY);

        const vx_size maxNumOfPoints = 2000;
        const vx_size maxNumOfPlanesVertices = 2000;
        vx_array filteredPoints = vxCreateArray(context, NVX_TYPE_POINT3F, maxNumOfPoints);
        vx_array planesVertices = vxCreateArray(context, NVX_TYPE_POINT3F, maxNumOfPlanesVertices);

        //
        // Run processing loop
        //

        vx_matrix model = vxCreateMatrix(context, VX_TYPE_FLOAT32, 4, 4);
        float eye_data[4*4] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        vxWriteMatrix(model, eye_data);

        nvxio::Render3D::PointCloudStyle pcStyle = {0, 12};
        nvxio::Render3D::PlaneStyle fStyle = {0, 10};

        GroundPlaneSmoother groundPlaneSmoother(7);

        /////////////////////////////////////////////

        Mat *opencv_frame;
        vx_image frameGray = vxCreateImage(context, 1280, 720, VX_DF_IMAGE_U8);


        /*intrins parameter*/



        ////////////////////////////////////////



        nvx::Timer totalTimer;
        totalTimer.tic();
        double proc_ms = 0;
        float yGroundPlane = 0;
        while (!eventData.shouldStop)
        {

            if (!eventData.pause)
            {

//                cv::Mat fran=cv::Mat::eye(100,100,CV_8UC3);
//                cv::cuda::GpuMat frames,frame1;
//                frames.upload(fran);
//                cv::cuda::cvtColor(frames,frame1,CV_BGR2GRAY);

                frameStatus = source->fetch(frame);//10ms


                if (frameStatus == nvxio::FrameSource::TIMEOUT)
                {
                    continue;
                }
                if (frameStatus == nvxio::FrameSource::CLOSED)
                {
                    if(noLoop) break;

                    if (!source->open())
                    {
                        std::cerr << "Failed to reopen the source" << std::endl;
                        break;
                    }

                    do
                    {
                        frameStatus = source->fetch(frame);
                    }
                    while (frameStatus == nvxio::FrameSource::TIMEOUT);

                    sfm->init(frame, mask, imuDataFile, frameDataFile);

                    fenceDetector.reset();

                    continue;
                }

                // Process
                nvx::Timer procTimer;
                procTimer.tic();

                opencv_frame = new Mat(1280, 720, CV_8U);
                NVXIO_SAFE_CALL( vxuColorConvert(context, frame, frameGray) );
                VX_to_CV_Image(&opencv_frame,frameGray);//3ms
                //std::cout << *opencv_frame << std::endl;



//               undistort(*opencv_frame,undistored,camera_matrix,dist_coeffs);

                if(m_recognizer.position_est(*opencv_frame))
                {
                    coorMatrix = m_recognizer.get_position();
                    std::cout << coorMatrix << std::endl;
                }
                delete opencv_frame;

//                sfm->track(frame, mask);
                proc_ms = procTimer.toc();
            }

            // Print performance results
//            sfm->printPerfs();

            if (!eventData.showPointCloud)
            {
                render3D->disableDefaultKeyboardEventCallback();
                render3D->putImage(frame);
            }
            else
            {
                render3D->enableDefaultKeyboardEventCallback();
            }

            filterPoints(sfm->getPointCloud(), filteredPoints);
            render3D->putPointCloud(filteredPoints, model, pcStyle);

            if (eventData.showFences)
            {
                fenceDetector.getFencePlaneVertices(filteredPoints, planesVertices);
                render3D->putPlanes(planesVertices, model, fStyle);
            }

            if (fullPipeline && eventData.showGP)
            {
                const float x1(-1.5), x2(1.5), z1(1), z2(4);

                vx_matrix gp = sfm->getGroundPlane();
                yGroundPlane = groundPlaneSmoother.getSmoothedY(gp, x1, z1);

                nvx_point3f_t pt[4] = {{x1, yGroundPlane, z1},
                                       {x1, yGroundPlane, z2},
                                       {x2, yGroundPlane, z2},
                                       {x2, yGroundPlane, z1}};

                vx_array gpPoints = vxCreateArray(context, NVX_TYPE_POINT3F, 4);
                vxAddArrayItems(gpPoints, 4, pt, sizeof(pt[0]));

                render3D->putPlanes(gpPoints, model, fStyle);
                vxReleaseArray(&gpPoints);
            }

            double total_ms = totalTimer.toc();

            // Add a delay to limit frame rate
//            app.sleepToLimitFPS(total_ms);

            total_ms = totalTimer.toc();
            totalTimer.tic();

            std::string state = createInfo(fullPipeline, proc_ms, total_ms, eventData);
            render3D->putText(state.c_str(), style);

            if (!render3D->flush())
            {
                eventData.shouldStop = true;
            }


        }

        //
        // Release all objects
        //
        vxReleaseImage(&frame);
        vxReleaseImage(&mask);
        vxReleaseMatrix(&model);
        vxReleaseArray(&filteredPoints);
        vxReleaseArray(&planesVertices);

    }
//    catch (const std::exception& e)
//    {
//        std::cerr << "Error: " << e.what() << std::endl;
//        return nvxio::Application::APP_EXIT_CODE_ERROR;
//    }

    return nvxio::Application::APP_EXIT_CODE_SUCCESS;

}
