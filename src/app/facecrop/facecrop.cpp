/*!
  @file   facecrop.cpp
  @author David Hirvonen
  @brief  Batch processing to parse and synthesize new face images from various databases.

  \copyright Copyright 2017 Elucideye, Inc. All rights reserved.
  \license{This project is released under the 3 Clause BSD License.}

*/

#include "drishti/core/drishti_stdlib_string.h" // android workaround
#include "drishti/core/Logger.h"
#include "drishti/core/make_unique.h"
#include "drishti/core/Parallel.h"
#include "drishti/core/LazyParallelResource.h"
#include "drishti/core/drishti_string_hash.h"
#include "drishti/core/string_utils.h"
#include "drishti/geometry/motion.h"
#include "drishti/testlib/drishti_cli.h"
#include "drishti/core/drishti_cv_cereal.h"

#include "landmarks/FACE.h"
#include "landmarks/MUCT.h"
#include "landmarks/HELEN.h"
#include "landmarks/BIOID.h"
#include "landmarks/LFW.h"
#include "landmarks/LFPW.h"
#include "landmarks/DRISHTI.h"
#include "landmarks/TWO.h"

#include "FaceSpecification.h"
#include "FaceJitterer.h"

#if defined(DRISHTI_USE_IMSHOW)
#  include "imshow/imshow.h"
#endif

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <cereal/archives/json.hpp>

#include "cxxopts.hpp"

/// ######################## >> FACE << #############################

struct FaceJittererMean : public FaceJitterer
{
    FaceJittererMean(const FACE::Table &table, const JitterParams &params, const FaceSpecification &face)
    : FaceJitterer(table, params, face) {}

    void updateMean(const std::vector<cv::Mat> &faces)
    {
        // update the face mean
        cv::Mat3f F(faces.front().size());
        for(auto &f : faces)
        {
            f.convertTo(F, CV_32FC3, 1.0f/255.0f);
            updateMean(F);
        }
    }
    
    void updateMean(const cv::Mat3f &f)
    {
        cumulativeMovingAverage(mu, f, ++count);
    }
  
    void cumulativeMovingAverage(cv::Mat3f &mu, const cv::Mat3f &x, int n)
    {
        if(mu.empty())
        {
            mu = x.clone();
        }
        else
        {
            mu += (x - mu) * (1.0 / static_cast<float>(n));
        }
    }
    
    cv::Mat3f mu;
    int count = 0;
};

using ImageVec=std::vector<cv::Mat>;
using FaceJittererMeanPtr = std::unique_ptr<FaceJittererMean>;
using FaceResourceManager = drishti::core::LazyParallelResource<std::thread::id, FaceJittererMeanPtr>;
static cv::Mat3f computeMeanFace(FaceResourceManager &manager);
static int saveDefaultConfigs(const std::string &sOutput, spdlog::logger &logger);
static void save(const ImageVec &faces, const std::string &dir, const std::string &filename, int index);

enum GroundTruthFormat
{
    MUCTFormat,
    FDDBFormat,
    HELENFormat,
    BIOIDFormat,
    LFWFormat,
    DRISHTIFormat,
    LFPWFormat,
    TWOFormat
};

#define SUPPORTED_FORMATS "muct,fddb,helen,bioid,lfw,drishti,lfpw,two"

static void previewFaceWithLandmarks(cv::Mat &image, const std::vector<cv::Point2f> &landmarks);

using string_hash::operator "" _hash;

int main(int argc, char* argv[])
{
    const auto argumentCount = argc;
    
    // Instantiate line logger:
    auto logger = drishti::core::Logger::create("drishti-facecrop");
    
    // ############################
    // ### Command line parsing ###
    // ############################
    
    std::string sInput;
    std::string sOutput;
    std::string sFormat;
    std::string sDirectory;
    std::string sExtension;
    std::string sFaceSpec;
    std::string sJitterIn;
    
    int sampleCount = 0;
    int threads = -1;
    bool doPreview = false;
    bool doBoilerplate = false;
    bool doPhotometricJitterOnly = false;

    cxxopts::Options options("drishti-facecrop", "Command line interface for facecrop object detection.");
    options.add_options()
        ("i,input", "Input file", cxxopts::value<std::string>(sInput))
        ("o,output", "Output directory", cxxopts::value<std::string>(sOutput))
        ("f,format", "Format:" SUPPORTED_FORMATS, cxxopts::value<std::string>(sFormat))
        ("d,directory", "Base (d)irectory", cxxopts::value<std::string>(sDirectory))
        ("s,specification", "Face specification", cxxopts::value<std::string>(sFaceSpec))
        ("j,jitter", "Jitter input parameters", cxxopts::value<std::string>(sJitterIn))
        ("n,number", "Number of output samples to generate", cxxopts::value<int>(sampleCount))
        ("b,boilerplate", "Write boilerplate config to output dir", cxxopts::value<bool>(doBoilerplate))
        ("e,extension", "Image filename extensions", cxxopts::value<std::string>(sExtension))
        ("p,preview", "Do preview", cxxopts::value<bool>(doPreview))
        ("0,zero", "Zero jitter model (photometric jitter only)", cxxopts::value<bool>(doPhotometricJitterOnly))
    
        // Output parameters:
        ("t,threads", "Thread count", cxxopts::value<int>(threads))
        ("h,help", "Print help message");
    
    options.parse(argc, argv);
    
    if((argumentCount <= 1) || options.count("help"))
    {
        std::cout << options.help({""}) << std::endl;
        return 0;
    }
    
    // ############################################
    // ### Command line argument error checking ###
    // ############################################

    // ### Directory
    if(sOutput.empty())
    {
        logger->error() << "Must specify output directory";
        return 1;
    }
    
    if(drishti::cli::directory::exists(sOutput, ".drishti-facecrop"))
    {
        std::string filename = sOutput + "/.drishti-facecrop";
        remove(filename.c_str());
    }
    else
    {
        logger->error() << "Specified directory " << sOutput << " does not exist or is not writeable";
        return 1;
    }
    
    // ### Input
    if(sInput.empty())
    {
        logger->error() << "Must specify input image or list of images";
        return 1;
    }
    if(!drishti::cli::file::exists(sInput))
    {
        logger->error() << "Specified input file does not exist or is not readable";
        return 1;
    }
    
    GroundTruthFormat format = HELENFormat;

    FACE::Table table;

    switch(string_hash::hash(sFormat))
    {
        case "two"_hash:
            format = TWOFormat;
            table = parseTWO(sInput);
            break;
        case "drishti"_hash:
            format = DRISHTIFormat;
            table = parseDRISHTI(sInput);
            break;
        case "lfw"_hash :
            format = LFWFormat;
            table = parseLFW(sInput);
            break;
        case "muct"_hash  :
            format = MUCTFormat;
            table = parseMUCT(sInput);
            break;
        case "helen"_hash :
            format = HELENFormat;
            table = parseHELEN(sInput);
            break;
        case "bioid"_hash :
            format = BIOIDFormat;
            table = parseBIOID(sInput);
            break;
        case "lfpw"_hash :
            format = LFPWFormat;
            table = parseLFPW(sInput);
            break;
        default :
            CV_Assert(false);
    }
    
    if(!sDirectory.empty())
    {
        if(sDirectory.back() != '/')
        {
            sDirectory += "/";
        }
        
        for(auto &l : table.lines)
        {
            l.filename = sDirectory + l.filename;
            if(!sExtension.empty())
            {
                l.filename += ".png";
            }
        }
    }
    
    if(table.lines.empty())
    {
        logger->error() << "Error: no images were found, please check input file and (optionally) base directory";
        return -1;
    }
    else
    { // Try simple image read sanity test for user feedback:
        if(cv::imread(table.lines.front().filename).empty())
        {
            logger->error() << "Error: unable to read input image, please check input file and (optionally) base directory";
            return -1;
        }
    }

    if(!drishti::cli::file::exists(sInput))
    {
        logger->error() << "Specified input file does not exist or is not readable";
        return 1;
    }
    
    if(doBoilerplate)
    {
        if(int code = saveDefaultConfigs(sOutput, *logger) < 0)
        {
            return code;
        }
    }
    
    JitterParams jitterParams;
    if(!sJitterIn.empty())
    {
        std::ifstream is(sJitterIn);
        if(is)
        {
            cereal::JSONInputArchive ia(is);
            typedef decltype(ia) Archive;
            ia(GENERIC_NVP("jitter", jitterParams));
        }
        else
        {
            logger->error() << "Error: unable to read input jitter parameters";
            return -1;
        }
    }
    
    // ###
    // ### Specification
    // ###

    FaceSpecification faceSpec; // { targetIod, {targetCenterX, targetCenterY},  {targetWidth, targetHeight} };
    if(sFaceSpec.empty())
    {
        logger->error() << "Error: must provide valid face specification";
        return -1;
    }
    else
    {
        std::ifstream is(sFaceSpec);
        if(is)
        {
            cereal::JSONInputArchive ia(is);
            typedef decltype(ia) Archive;
            ia(GENERIC_NVP("face", faceSpec));
        }
        else
        {
            logger->error() << "Error: unable to read face specification file: " << sFaceSpec;
            return -1;
        }
    }

    // Determine samples:
    std::vector<int> repeat(table.lines.size(), 1);
    if(sampleCount > 0)
    {
        std::fill(begin(repeat), end(repeat), 0);
        std::vector<int> samples(sampleCount);
        cv::RNG().fill(samples, 0, 0, table.lines.size());
        for(const auto &i : samples)
        {
            repeat[i]++;
        }
    }

    FaceResourceManager manager = [&]()
    {
        logger->info() << "Create resource...";
        return drishti::core::make_unique<FaceJittererMean>(table, jitterParams, faceSpec);
    };

    drishti::core::ParallelHomogeneousLambda harness = [&](int i)
    {
        // Get thread specific segmenter lazily:
        auto tid = std::this_thread::get_id();
        
        auto &jitterer = manager[tid];
        assert(jitterer.get());
        
        // Load current image
        logger->info() << table.lines[i].filename << " = " << repeat[i];
        
        if(repeat[i] > 0)
        {
            cv::Mat image = cv::imread(table.lines[i].filename, cv::IMREAD_COLOR);
            
            if(!image.empty())
            {
                std::vector<cv::Mat> faces { (*jitterer)(image, table.lines[i].points, false, true) };
                for(int j = 1; j < repeat[i]; j++)
                {
                    faces.push_back((*jitterer)(image, table.lines[i].points, !doPhotometricJitterOnly, true));
                }
                
                jitterer->updateMean(faces);

                if(!sOutput.empty())
                {
                    save(faces, sOutput, table.lines[i].filename, i);
                }
                
#if defined(DRISHTI_USE_IMSHOW)
                if(doPreview)
                {
                    cv::Mat canvas = image.clone();
                    previewFaceWithLandmarks(canvas, table.lines[i].points);
                    glfw::imshow("facecrop:image", canvas);
                    
                    cv::hconcat(faces, canvas);
                    glfw::imshow("facecrop:jitter", canvas);
                    glfw::imshow("facecrop::mu", jitterer->mu);
                    
                    glfw::waitKey(0);
                }
#endif
            }
        }
    };
    
    if(threads == 1 || threads == 0 || doPreview)
    {
        harness({0,static_cast<int>(table.lines.size())});
    }
    else
    {
        cv::parallel_for_({0,static_cast<int>(table.lines.size())}, harness, std::max(threads, -1));
    }

    { // Save the mean face image:
        cv::Mat mu = computeMeanFace(manager);
        if(!sOutput.empty())
        {
            cv::Mat tmp;
            mu.convertTo(tmp, CV_8UC3, 255.0);
            cv::imwrite(sOutput + "/mean.png", tmp);
        }

#if defined(DRISHTI_USE_IMSHOW)        
        if(doPreview)
        {
            glfw::imshow("facecrop::mu", mu);
            glfw::waitKey(0);
        }
#endif
    }
}

// ### utility ###

static int saveDefaultJitter(const std::string &sOutput, spdlog::logger &logger)
{
    // Write default jitter parameters:
    std::ofstream os(sOutput + "/jitter.json");
    if(os)
    {
        cereal::JSONOutputArchive oa(os);
        typedef decltype(oa) Archive;
        oa(GENERIC_NVP("jitter", JitterParams()));
    }
    else
    {
        logger.error() << "Error: unable to write default jitter parameters";
        return -1;
    }
    return 0;
}

static int saveDefaultFaceSpec(const std::string &sOutput, spdlog::logger &logger)
{
    // Write default face specification:
    std::ofstream os(sOutput + "/face.json");
    if(os)
    {
        cereal::JSONOutputArchive oa(os);
        typedef decltype(oa) Archive;
        oa(GENERIC_NVP("face", FaceSpecification()));
    }
    else
    {
        logger.error() << "Error: unable to write default face specification parameters";
        return -1;
    }
    return 0;
}

static int saveDefaultConfigs(const std::string &sOutput, spdlog::logger &logger)
{
    if(int code = saveDefaultJitter(sOutput, logger) != 0)
    {
        return code;
    }
    if(int code = saveDefaultFaceSpec(sOutput, logger) != 0)
    {
        return code;
    }
    return 0;
}

static cv::Mat3f computeMeanFace(FaceResourceManager &manager)
{
    cv::Mat3f mu;
    int count = 0;
    
    for(const auto &j : manager.getMap())
    {
        count += j.second->count;
    }
    
    for(const auto &j : manager.getMap())
    {
        if(mu.empty())
        {
            mu = (float(j.second->count)/count) * j.second->mu;
        }
        else
        {
            mu += (float(j.second->count)/count) * j.second->mu;
        }
    }
    
    return mu;
}

static void save(const ImageVec &faces, const std::string &dir, const std::string &filename, int index)
{
    for(int i = 0; i < faces.size(); i++)
    {
        std::stringstream ss;
        ss << std::setfill('0') << std::setw(6) << index << "_" << std::setw(2) << i;
        std::string base = drishti::core::basename(filename);
        std::string sOutput = dir + "/" + ss.str() + "_" + base + ".png";
        cv::imwrite(sOutput, faces[i]);
    }
}

static void previewFaceWithLandmarks(cv::Mat &image, const std::vector<cv::Point2f> &landmarks)
{
    for(const auto &p : landmarks)
    {
        cv::circle(image, p, 2, {0,255,0}, -1, 8);
    }
}