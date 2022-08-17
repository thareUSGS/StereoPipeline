// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__


/// \file jitter_adjust.cc
///
/// Use n adjustments for every camera, placed at several lines in the image
// with interpolation between them. The pdf doc has more info.

#include <vw/BundleAdjustment/ControlNetwork.h>
#include <vw/BundleAdjustment/ControlNetworkLoader.h>
#include <vw/Core/Stopwatch.h>
#include <vw/Cartography/CameraBBox.h>

#include <asp/Core/Macros.h>
#include <asp/Core/StereoSettings.h>
#include <asp/Core/BundleAdjustUtils.h>
#include <asp/Core/IpMatchingAlgs.h> // Lightweight header for matching algorithms
#include <asp/Sessions/CameraUtils.h>
#include <asp/Camera/CsmModel.h>

#include <usgscsm/UsgsAstroLsSensorModel.h>
#include <usgscsm/Utilities.h>

#include <xercesc/util/PlatformUtils.hpp>

#include <ceres/ceres.h>
#include <ceres/loss_function.h>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace vw;
using namespace vw::math;


namespace asp {

const int NUM_XYZ_PARAMS  = 3;
const int NUM_QUAT_PARAMS = 4;
const int PIXEL_SIZE      = 2;

const double g_big_pixel_value = 1000.0;  // don't make this too big

// An error function minimizing the error of projecting an xyz point
// into a given camera pixel. The variables of optimization are a
// portion of the position and quaternion variables affected by this.
  
struct pixelReprojectionError {
  pixelReprojectionError(vw::Vector2 const& observation, UsgsAstroLsSensorModel* ls_model,
                         int begQuatIndex, int endQuatIndex, int begPosIndex, int endPosIndex):
    m_observation(observation),
    m_begQuatIndex(begQuatIndex), m_endQuatIndex(endQuatIndex),
    m_begPosIndex(begPosIndex),   m_endPosIndex(endPosIndex),
    m_ls_model(ls_model){}

  // Call to work with ceres::DynamicCostFunction.
  bool operator()(double const * const * parameters, double * residuals) const {

    try {
      // Make a copy of the model, as we will update quaternion and position values
      // that are being modified now. This may be expensive.
      UsgsAstroLsSensorModel cam = *m_ls_model;

      // Update the relevant quaternions in the local copy
      int shift = 0;
      for (int qi = m_begQuatIndex; qi < m_endQuatIndex; qi++) {
        for (int coord = 0; coord < NUM_QUAT_PARAMS; coord++) {
          cam.m_quaternions[NUM_QUAT_PARAMS * qi + coord]
            = parameters[qi + shift - m_begQuatIndex][coord];
        }
      }

      // Same for the positions. Note how we move forward in the parameters array,
      // as this is after the quaternions
      shift += (m_endQuatIndex - m_begQuatIndex);
      for (int pi = m_begPosIndex; pi < m_endPosIndex; pi++) {
        for (int coord = 0; coord < NUM_XYZ_PARAMS; coord++) {
          cam.m_positions[NUM_XYZ_PARAMS * pi + coord]
            = parameters[pi + shift - m_begPosIndex][coord];
        }
      }

      // Move forward in the array of parameters, then recover the triangulated point
      shift += (m_endPosIndex - m_begPosIndex);
      csm::EcefCoord P;
      P.x = parameters[shift][0];
      P.y = parameters[shift][1];
      P.z = parameters[shift][2];

      // Project in the camera with high precision
      double desired_precision = 1e-12;
      csm::ImageCoord imagePt = cam.groundToImage(P, desired_precision);

      // Convert to what ASP expects
      vw::Vector2 pix;
      asp::fromCsmPixel(pix, imagePt);

      residuals[0] = pix[0] - m_observation[0];
      residuals[1] = pix[1] - m_observation[1];
      
    } catch (std::exception const& e) {
      residuals[0] = g_big_pixel_value;
      residuals[1] = g_big_pixel_value;
      return true; // accept the solution anyway
    }
    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(vw::Vector2 const& observation,
                                     UsgsAstroLsSensorModel* ls_model,
                                     int begQuatIndex, int endQuatIndex,
                                     int begPosIndex, int endPosIndex){

    // TODO(oalexan1): Try using here the analytical cost function
    ceres::DynamicNumericDiffCostFunction<pixelReprojectionError>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<pixelReprojectionError>
      (new pixelReprojectionError(observation, ls_model,
                                  begQuatIndex, endQuatIndex,
                                  begPosIndex, endPosIndex));

    // The residual size is always the same.
    cost_function->SetNumResiduals(PIXEL_SIZE);

    // Add a parameter block for each quaternion and each position
    for (int it = begQuatIndex; it < endQuatIndex; it++)
      cost_function->AddParameterBlock(NUM_QUAT_PARAMS);
    for (int it = begPosIndex; it < endPosIndex; it++)
      cost_function->AddParameterBlock(NUM_XYZ_PARAMS);

    // Add a parameter block for the xyz point
    cost_function->AddParameterBlock(NUM_XYZ_PARAMS);
    
    return cost_function;
  }

private:
  Vector2 m_observation; // The pixel observation for this camera/point pair
  UsgsAstroLsSensorModel* m_ls_model;
  int m_begQuatIndex, m_endQuatIndex;
  int m_begPosIndex, m_endPosIndex;
}; // End class pixelReprojectionError

struct Options : public vw::GdalWriteOptions {
  std::string out_prefix, stereo_session, input_prefix, match_files_prefix, clean_match_files_prefix;
  std::string ref_dem;
  int overlap_limit, min_matches, max_pairwise_matches, num_iterations;
  bool match_first_to_last, single_threaded_cameras;
  double min_triangulation_angle, max_init_reproj_error, robust_threshold, parameter_tolerance;
  double ref_dem_weight, ref_dem_robust_thresh;
  std::vector<std::string> image_files, camera_files;
  std::vector<boost::shared_ptr<vw::camera::CameraModel>> camera_models;
};
    
void handle_arguments(int argc, char *argv[], Options& opt) {

  po::options_description general_options("");
  general_options.add_options()
    ("output-prefix,o",  po::value(&opt.out_prefix), "Prefix for output filenames.")
    ("session-type,t",   po::value(&opt.stereo_session)->default_value(""),
     "Select the stereo session type to use for processing. Usually the program "
     "can select this automatically by the file extension, except for xml cameras. "
     "See the doc for options.")
    ("input-adjustments-prefix",  po::value(&opt.input_prefix),
     "Prefix to read initial adjustments from, written by bundle_adjust.")
    ("match-first-to-last",
     po::value(&opt.match_first_to_last)->default_value(false)->implicit_value(true),
     "Match the last several images to several first images by extending the logic of "
     "--overlap-limit past the last image to the earliest ones.")
    ("overlap-limit",        po::value(&opt.overlap_limit)->default_value(0),
     "Limit the number of subsequent images to search for matches to the current image "
     "to this value. By default match all images.")
    ("match-files-prefix",  po::value(&opt.match_files_prefix)->default_value(""),
     "Use the match files from this prefix instead of the current output prefix.")
    ("clean-match-files-prefix",  po::value(&opt.clean_match_files_prefix)->default_value(""),
     "Use as input match files the *-clean.match files from this prefix.")
    ("min-matches", po::value(&opt.min_matches)->default_value(30),
     "Set the minimum  number of matches between images that will be considered.")
    ("max-pairwise-matches", po::value(&opt.max_pairwise_matches)->default_value(10000),
     "Reduce the number of matches per pair of images to at most this "
     "number, by selecting a random subset, if needed. This happens "
     "when setting up the optimization, and before outlier filtering.")
    ("min-triangulation-angle", po::value(&opt.min_triangulation_angle)->default_value(0.1),
     "The minimum angle, in degrees, at which rays must meet at a triangulated point to "
     "accept this point as valid. It must be a positive value.")
    ("max-initial-reprojection-error", po::value(&opt.max_init_reproj_error)->default_value(5),
     "Filter as outliers triangulated points project using initial cameras with error more than "
     "this, measured in pixels. Since jitter corrections are supposed to be small and cameras "
     "bundle-adjusted by now, this value should be small.")
    ("robust-threshold", po::value(&opt.robust_threshold)->default_value(0.5),
     "Set the threshold for the Cauchy robust cost function. Increasing this makes "
     "the solver focus harder on the larger errors.")
    ("parameter-tolerance",  po::value(&opt.parameter_tolerance)->default_value(1e-12),
     "Stop when the relative error in the variables being optimized is less than this.")
    ("num-iterations",       po::value(&opt.num_iterations)->default_value(500),
     "Set the maximum number of iterations.")
    ("reference-dem",  po::value(&opt.ref_dem)->default_value(""),
     "If specified, constrain every ground point where rays from matching pixels intersect "
     "to be not too far from the average of intersections of those rays with this DEM.")
    ("reference-dem-weight", po::value(&opt.ref_dem_weight)->default_value(1.0),
     "Multiply the xyz differences for the --reference-dem option by this weight.")
    ("reference-dem-robust-threshold", po::value(&opt.ref_dem_robust_thresh)->default_value(0.5),
     "Use this robust threshold for the weighted xyz differences.");
  
  general_options.add(vw::GdalWriteOptionsDescription(opt));
    
  po::options_description positional("");
  positional.add_options()
    ("input-files", po::value(&opt.image_files));
  
  po::positional_options_description positional_desc;
  positional_desc.add("input-files", -1);

  std::string usage("<images> <cameras> -o <output prefix> [options]");
  bool allow_unregistered = false;
  std::vector<std::string> unregistered;
  po::variables_map vm =
    asp::check_command_line(argc, argv, opt, general_options, general_options,
                            positional, positional_desc, usage,
                             allow_unregistered, unregistered);

  std::vector<std::string> inputs = opt.image_files;
  bool ensure_equal_sizes = true;
  asp::separate_images_from_cameras(inputs,
                                    opt.image_files, opt.camera_files, // outputs
                                    ensure_equal_sizes); 

  // Throw if there are duplicate camera file names.
  asp::check_for_duplicates(opt.image_files, opt.camera_files, opt.out_prefix);
  
  const int num_images = opt.image_files.size();
  
  // Sanity check
  if (opt.image_files.size() != opt.camera_files.size())
    vw_throw(ArgumentErr() << "Must have as many cameras as  have images.\n");
  
  if (opt.image_files.empty())
    vw_throw(ArgumentErr() << "Missing input image files.\n");
  
  if (opt.overlap_limit < 0)
    vw_throw(ArgumentErr() << "Must allow search for matches between "
             << "at least each image and its subsequent one.\n");
  
  // By default, try to match all of the images
  if (opt.overlap_limit == 0)
    opt.overlap_limit = opt.image_files.size();
  
  if (int(opt.match_files_prefix.empty()) + int(opt.clean_match_files_prefix.empty()) != 1) 
    vw_throw(ArgumentErr() << "Must specify precisely one of: --match-files-prefix, "
             << "--clean-match-files-prefix.\n");

  if (opt.input_prefix.empty())
    vw_throw(ArgumentErr() << "Must specify --input-adjustments-prefix.\n");

  if (opt.max_init_reproj_error <= 0.0)
    vw_throw(ArgumentErr() << "Must have a positive --max-initial-reprojection-error.\n");

  return;
}

void compute_residuals(Options const& opt,
                       ceres::Problem & problem,
                       // Output
                       std::vector<double> & residuals) {

  double cost = 0.0;
  ceres::Problem::EvaluateOptions eval_options;
  eval_options.apply_loss_function = false;
  if (opt.single_threaded_cameras)
    eval_options.num_threads = 1; // ISIS must be single threaded!
  else
    eval_options.num_threads = opt.num_threads;
  
  problem.Evaluate(eval_options, &cost, &residuals, 0, 0);
}
  
void run_jitter_solve(int argc, char* argv[]) {

  // Parse arguments and perform validation
  Options opt;
  handle_arguments(argc, argv, opt);

  bool approximate_pinhole_intrinsics = false;
  asp::load_cameras(opt.image_files, opt.camera_files, opt.out_prefix, opt,  
                    approximate_pinhole_intrinsics,  
                    // Outputs
                    opt.stereo_session,  // may change
                    opt.single_threaded_cameras,  
                    opt.camera_models);

  // Apply the input adjustments to the CSM cameras. Get pointers to the underlying
  // linescan cameras, as need to manipulate those directly.
  std::vector<UsgsAstroLsSensorModel*> ls_cams;
  for (size_t it = 0; it < opt.camera_models.size(); it++) {
    vw::camera::CameraModel * base_cam = vw::camera::unadjusted_model(opt.camera_models[it]).get();
    asp::CsmModel * csm_cam = dynamic_cast<asp::CsmModel*>(base_cam);
    if (csm_cam == NULL)
      vw_throw(ArgumentErr() << "Expecting the cameras to be of CSM type.\n");
    
    std::string adjust_file
      = asp::bundle_adjust_file_name(opt.input_prefix, opt.image_files[it],
                                     opt.camera_files[it]);
    vw_out() << "Reading input adjustment: " << adjust_file << std::endl;

    // This modifies opt.camera_models
    vw::camera::AdjustedCameraModel adj_cam(vw::camera::unadjusted_model(opt.camera_models[it]));
    adj_cam.read(adjust_file);
    vw::Matrix4x4 ecef_transform = adj_cam.ecef_transform();
    csm_cam->applyTransform(ecef_transform);

    //std::cout << "transform " << ecef_transform << std::endl;
    
    UsgsAstroLsSensorModel * ls_cam
      = dynamic_cast<UsgsAstroLsSensorModel*>((csm_cam->m_csm_model).get());
    if (ls_cam == NULL)
      vw_throw(ArgumentErr() << "Expecting the cameras to be of CSM linescan type.\n");
    std::cout << "--loaded " << ls_cam << std::endl;
    ls_cams.push_back(ls_cam);
  }

  // Quantities that are not needed but are part of the API below
  bool got_est_cam_positions = false;
  double position_filter_dist = -1.0;
  std::vector<vw::Vector3> estimated_camera_gcc;
  std::set<std::pair<std::string, std::string>> overlap_list;

  // Make a list of all the image pairs to find matches for
  std::vector<std::pair<int,int>> all_pairs;
  asp::determine_image_pairs(// Inputs
                             opt.overlap_limit, opt.match_first_to_last,  
                             opt.image_files, 
                             got_est_cam_positions, position_filter_dist,
                             estimated_camera_gcc, overlap_list,
                             // Output
                             all_pairs);

  // Load match files
  std::map<std::pair<int, int>, std::string> match_files;
  for (size_t k = 0; k < all_pairs.size(); k++) {
    int i = all_pairs[k].first;
    int j = all_pairs[k].second;
    std::string const& image1_path  = opt.image_files[i];  // alias
    std::string const& image2_path  = opt.image_files[j];  // alias
    std::string const& camera1_path = opt.camera_files[i]; // alias
    std::string const& camera2_path = opt.camera_files[j]; // alias

    // Load match files from a different source
    bool allow_missing_match_file = true; 
    std::string match_filename 
      = asp::match_filename(opt.clean_match_files_prefix, opt.match_files_prefix,  
                            opt.out_prefix, image1_path, image2_path, allow_missing_match_file);
    match_files[std::make_pair(i, j)] = match_filename;
  }

  // Build control network and perform triangulation with adjusted input cameras
  ba::ControlNetwork cnet("jitter_solve");
  bool triangulate_control_points = true;
  double forced_triangulation_distance = -1.0;
  bool success = vw::ba::build_control_network(triangulate_control_points,
                                               cnet, // output
                                               opt.camera_models, opt.image_files,
                                               match_files, opt.min_matches,
                                               opt.min_triangulation_angle*(M_PI/180.0),
                                               forced_triangulation_distance,
                                               opt.max_pairwise_matches);
  if (!success)
    vw_throw(ArgumentErr()
             << "Failed to build a control network. Consider removing "
             << "all .vwip and .match files and increasing "
             << "the number of interest points per tile using "
             << "--ip-per-tile, or decreasing --min-matches.\n");

  int num_cameras = opt.camera_models.size();
  if (num_cameras < 2)
    vw_throw(ArgumentErr() << "Expecting at least two input cameras.\n");
    
  // Tri_Points
  int num_tri_points = cnet.size();
  std::cout << "--number of tri_points " << num_tri_points << std::endl;
  if (num_tri_points == 0)
   vw_throw(ArgumentErr() << "No triangulated ground points were found.\n"); 
  std::vector<double> tri_points_vec(num_tri_points*NUM_XYZ_PARAMS, 0.0);
  for (int ipt = 0; ipt < num_tri_points; ipt++){
    for (int q = 0; q < NUM_XYZ_PARAMS; q++){
      tri_points_vec[ipt*NUM_XYZ_PARAMS + q] = cnet[ipt].position()[q];
    }
  }
  double* tri_points = &tri_points_vec[0];

  // TODO(oalexan1): Is it possible to avoid using CRNs?
  vw::ba::CameraRelationNetwork<vw::ba::JFeature> crn;
  crn.read_controlnetwork(cnet);
  
  if ((int)crn.size() != opt.camera_models.size()) 
    vw_throw(ArgumentErr() << "Book-keeping error, the size of CameraRelationNetwork "
             << "must equal the number of images.\n");

  // TODO(oalexan1): Make this into a function
  // Must flag as outliers points with initial reprojection error bigger than
  // a certain amount.
  std::set<int> outliers;
  for (int icam = 0; icam < (int)crn.size(); icam++) {

    for (auto fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++) {
      
      // The index of the triangulated point
      int ipt = (**fiter).m_point_id;
      
      VW_ASSERT(icam < num_cameras, ArgumentErr() << "Out of bounds in the number of cameras.");
      VW_ASSERT(ipt < num_tri_points, ArgumentErr() << "Out of bounds in the number of points.");

      if (outliers.find(ipt) != outliers.end()) {
        // Is an outlier
        continue;
      }
      
      // The observed value for the projection of point with index ipt into
      // the camera with index icam.
      Vector2 observation = (**fiter).m_location;

      // Ideally this point projects back to the pixel observation.
      double * tri_point = tri_points + ipt * NUM_XYZ_PARAMS;
      
      vw::VectorProxy<double, 3> P(tri_point); // alias
      vw::Vector2 pix;
      try {
        pix = opt.camera_models[icam]->point_to_pixel(P);
        bool is_good = (norm_2(pix - observation) <= opt.max_init_reproj_error);
        if (!is_good) { // this checks for NaN too
          outliers.insert(ipt);
          continue;
        }
      } catch(...) {
        outliers.insert(ipt);
        continue;
      }
    }
  }

  // Set up the cost function
  // TODO(oalexan1): Make this into a function
  ceres::Problem problem;

  for (int icam = 0; icam < (int)crn.size(); icam++) {

    for (auto fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++) {
      
      // The index of the 3D point
      int ipt = (**fiter).m_point_id;
      
      if (outliers.find(ipt) != outliers.end())
        continue; // Skip outliers
      
      // The observed value for the projection of point with index ipt into
      // the camera with index icam.
      Vector2 observation = (**fiter).m_location;

      // Ideally this point projects back to the pixel observation.
      double * tri_point = tri_points + ipt * NUM_XYZ_PARAMS;

      // Must grow the number of quaternions and positions a bit
      // because during optimization the 3D point and corresponding
      // pixel may move somewhat.
      double line_extra = opt.max_init_reproj_error + 5.0; // add some more just in case
      csm::ImageCoord imagePt1, imagePt2;
      asp::toCsmPixel(observation - Vector2(0.0, line_extra), imagePt1);
      asp::toCsmPixel(observation + Vector2(0.0, line_extra), imagePt2);
      double time1 = ls_cams[icam]->getImageTime(imagePt1);
      double time2 = ls_cams[icam]->getImageTime(imagePt2);

      // Handle quaternions. We follow closely the conventions for UsgsAstroLsSensorModel.
      int numQuatPerObs = 8; // Max num of quaternions used in pose interpolation 
      int numQuat       = ls_cams[icam]->m_quaternions.size() / NUM_QUAT_PARAMS;
      double quatT0     = ls_cams[icam]->m_t0Quat;
      double quatDt     = ls_cams[icam]->m_dtQuat;

      // Starting and ending quat index (ending is exclusive). Based on lagrangeInterp().
      int qindex1      = static_cast<int>((time1 - quatT0) / quatDt);
      int qindex2      = static_cast<int>((time2 - quatT0) / quatDt);
      int begQuatIndex = std::min(qindex1, qindex2) - numQuatPerObs / 2 + 1;
      int endQuatIndex = std::max(qindex1, qindex2) + numQuatPerObs / 2 + 1;

      // Keep in bounds
      begQuatIndex = std::max(0, begQuatIndex);
      endQuatIndex = std::min(endQuatIndex, numQuat);
      if (begQuatIndex >= endQuatIndex) // Must not happen 
        vw_throw(ArgumentErr() << "Book-keeping error for pixel: " << observation << ".\n"); 

      // Same for positions
      int numPosPerObs = 8;
      int numPos       = ls_cams[icam]->m_positions.size() / NUM_XYZ_PARAMS;
      double posT0     = ls_cams[icam]->m_t0Ephem;
      double posDt     = ls_cams[icam]->m_dtEphem;
      
      // Starting and ending pos index (ending is exclusive). Based on lagrangeInterp().
      int pindex1 = static_cast<int>((time1 - posT0) / posDt);
      int pindex2 = static_cast<int>((time2 - posT0) / posDt);
      int begPosIndex = std::min(pindex1, pindex2) - numPosPerObs / 2 + 1;
      int endPosIndex = std::max(pindex1, pindex2) + numPosPerObs / 2 + 1;

      // Keep in bounds
      begPosIndex = std::max(0, begPosIndex);
      endPosIndex = std::min(endPosIndex, numPos);
      if (begPosIndex >= endPosIndex) // Must not happen 
        vw_throw(ArgumentErr() << "Book-keeping error for pixel: " << observation << ".\n"); 

      ceres::CostFunction* pixel_cost_function =
        pixelReprojectionError::Create(observation, ls_cams[icam],
                                       begQuatIndex, endQuatIndex,
                                       begPosIndex, endPosIndex);
      ceres::LossFunction* pixel_loss_function = new ceres::CauchyLoss(opt.robust_threshold);
      
      //std::cout << "--indices " << qindex1 << ' ' << qindex2 << ' ' << qindex2-qindex1 << std::endl;
      //std::cout << "--line samp 1 " << imagePt1.line << ' ' << imagePt1.samp << std::endl;
      //std::cout << "--line samp 2 " << imagePt2.line << ' ' << imagePt2.samp << std::endl;
      //std::cout << "--beg end quat index " << begQuatIndex << ' ' << endQuatIndex << std::endl;
      //std::cout << "--beg end pos index " << begPosIndex << ' ' << endPosIndex << std::endl;

      // The variable of optimization are camera quaternions and positions stored in the
      // camera models, and the triangulated point.
      std::vector<double*> vars;
      for (int it = begQuatIndex; it < endQuatIndex; it++)
        vars.push_back(&ls_cams[icam]->m_quaternions[it * NUM_QUAT_PARAMS]);
      for (int it = begPosIndex; it < endPosIndex; it++)
        vars.push_back(&ls_cams[icam]->m_positions[it * NUM_XYZ_PARAMS]);
      vars.push_back(tri_point);
      problem.AddResidualBlock(pixel_cost_function, pixel_loss_function, vars);
    }
  }

#if 0
  std::string dem_file = "tmp.tif";
  vw::cartography::GeoReference dem_georef;
  vw::ImageViewRef<vw::PixelMask<double>> interp_dem;
  asp::create_interp_dem(dem_file, dem_georef, interp_dem);
  
  double* point;
  // Areas that have no underlying DEM are not put any
  // constraints. The user can take advantage of that to put
  // constraints only in parts of the image where desired.
  if (asp::update_point_from_dem(point, dem_georef, interp_dem)) {
    // This overwrites the point! 
  }
  
  // TODO(oalexan1): Make this into a function
  ImageViewRef<PixelMask<double>> dem;
  if (opt.ref_dem != "") {
    vw_out() << "Loading reference DEM: " << opt.ref_dem << std::endl;
    double ref_dem_nodata = -std::numeric_limits<float>::max(); // note we use a float nodata
    if (vw::read_nodata_val(opt.ref_dem, ref_dem_nodata))
      vw_out() << "Found reference DEM nodata value: " << ref_dem_nodata << std::endl;
    
    ImageViewRef<PixelMask<double>> dem
      = create_mask(DiskImageView<double>(opt.ref_dem), ref_dem_nodata);

    vw::cartography::GeoReference dem_georef;
    if (!vw::cartography::read_georeference(dem_georef, opt.ref_dem))
      vw_throw(ArgumentErr() << "Could not read a georeference from: " << opt.ref_dem << ".\n");

    // Find the average intersections of rays corresponding to matching pixels with the DEM
    // TODO(oalexan1): Make this into a function
    std::vector<Vector3> dem_xyz_vec(num_tri_points);
    std::vector<int> dem_xyz_count(num_tri_points, 0);
    for (int icam = 0; icam < (int)crn.size(); icam++) {

      for (auto fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++) {
        
        // The index of the 3D point
        int ipt = (**fiter).m_point_id;
        
        if (outliers.find(ipt) != outliers.end())
          continue; // Skip outliers
        
        // The observed value for the projection of point with index ipt into
        // the camera with index icam.
        Vector2 observation = (**fiter).m_location;
        
        // Ideally this point projects back to the pixel observation.
        double * tri_point = tri_points + ipt * NUM_XYZ_PARAMS;
        Vector3 xyz_guess(tri_point[0], tri_point[1], tri_point[2]);

        bool treat_nodata_as_zero = false;
        bool has_intersection = false;
        double height_error_tol = 0.001; // 1 mm should be enough
        double max_abs_tol      = 1e-14; // abs cost fun change b/w iterations
        double max_rel_tol      = 1e-14;
        int num_max_iter        = 100;
        // This is so slow!
        Vector3 dem_xyz = vw::cartography::camera_pixel_to_dem_xyz
          (opt.camera_models[icam]->camera_center(observation),
           opt.camera_models[icam]->pixel_to_vector(observation),
           dem, dem_georef, treat_nodata_as_zero, has_intersection,
           height_error_tol, max_abs_tol, max_rel_tol, num_max_iter, xyz_guess);

        if (!has_intersection) 
          continue;

        std::cout << "--intersection " << dem_xyz << ' ' << xyz_guess << ' ' << dem_xyz - xyz_guess
                  << std::endl;

        dem_xyz_vec[ipt] += dem_xyz;
        dem_xyz_count[ipt]++;
      }
    }
  }
#endif
  
  // Solve the problem
  ceres::Solver::Options options;
  options.gradient_tolerance  = 1e-16;
  options.function_tolerance  = 1e-16;
  options.parameter_tolerance = opt.parameter_tolerance; // default is 1e-12
  
  options.max_num_iterations                = opt.num_iterations;
  options.max_num_consecutive_invalid_steps = std::max(5, opt.num_iterations/5); // try hard
  options.minimizer_progress_to_stdout      = true;

  if (opt.single_threaded_cameras)
    options.num_threads = 1;
  else
    options.num_threads = opt.num_threads;

  std::cout << "--num threads " << opt.num_threads << std::endl;

  std::vector<double> residuals;
  compute_residuals(opt, problem, residuals);
  
  //for (size_t it = 0; it < residuals.size(); it++) {
  //  std::cout << "--res " << residuals[it] << std::endl;
  // }

  vw_out() << "Starting the Ceres optimizer." << std::endl;
  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);
  //double final_cost = summary.final_cost;
  vw_out() << summary.FullReport() << "\n";
  if (summary.termination_type == ceres::NO_CONVERGENCE) 
    vw_out() << "Found a valid solution, but did not reach the actual minimum.\n";
  
  //   for (int icam = 0; icam < num_cameras; icam++) {
  //     std::cout << "--camera " << icam << std::endl;
  //     std::cout << "num quaternions " << ls_cams[icam]->m_quaternions.size() << std::endl;
  //     std::cout << "num positions " << ls_cams[icam]->m_positions.size() << std::endl;
  //     std::cout << "num velocities " << ls_cams[icam]->m_velocities.size() << std::endl;
  //     std::cout << "--covariance size " << ls_cams[icam]->m_covariance.size() << std::endl;
  //     std::cout << "--sun position " << ls_cams[icam]->m_sunPosition.size() << std::endl;
  //     std::cout << "--velocity " << ls_cams[icam]->m_sunVelocity.size() << std::endl;
  //     std::cout << "--m_intTimeLines " << ls_cams[icam]->m_intTimeLines.size() << std::endl;
  //     std::cout << "--m_intTimeStartTimes " << ls_cams[icam]->m_intTimeStartTimes.size() << std::endl;
  //     std::cout << "--m_intTimes " << ls_cams[icam]->m_intTimes.size() << std::endl;
  //   }
  // TODO(oalexan1): Wipe unnecessary things from the linescan model to avoid copying
  // unneeded data every single time Ceres does a numerical differentiation.

  return;
}

} // end namespace asp

  
int main(int argc, char* argv[]) {

  try {
    xercesc::XMLPlatformUtils::Initialize();
    
    asp::run_jitter_solve(argc, argv);
    
    xercesc::XMLPlatformUtils::Terminate();
  } ASP_STANDARD_CATCHES;

  return 0;
}