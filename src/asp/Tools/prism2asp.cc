// __BEGIN_LICENSE__
//  Copyright (c) 2009-2024, United States Government as represented by the
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

// Parse PRISM data and produce CSM camera files
// Reference: https://elib.dlr.de/57440/1/Schneider.pdf

#include <asp/Core/Common.h>
#include <asp/Core/Macros.h>
#include <asp/Camera/PRISM_XML.h>

// TODO(oalexan1): Many of these need to go when the code is modularized
#include <asp/Sessions/StereoSession.h>
#include <asp/Sessions/StereoSessionFactory.h>
#include <asp/Camera/CsmModel.h>
#include <asp/Camera/LinescanUtils.h>
#include <asp/Camera/CsmUtils.h>
#include <asp/Camera/CsmModelFit.h>
#include <asp/Camera/SatSim.h>
#include <asp/Core/SatSimBase.h>
#include <asp/Core/CameraTransforms.h>

#include <vw/FileIO/DiskImageView.h>
#include <vw/Cartography/Datum.h>
#include <vw/Cartography/GeoReference.h>
#include <vw/Cartography/GeoReferenceBaseUtils.h>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

struct Options: public vw::GdalWriteOptions {
  std::string dim_file, csm_file, dem;
  int ccd;

  // Constructor
  Options() {}
};

void handle_arguments(int argc, char *argv[], Options& opt) {

  po::options_description general_options("");
  general_options.add_options()
    ("dim", po::value(&opt.dim_file)->default_value(""),
     "The input PRISM .DIMA file.")
    ("ccd", po::value(&opt.ccd)->default_value(-1),
     "The CCD id to use. Must be between 1 and 6 or 8, depending on view (F, N, A).")
    ("csm", po::value(&opt.csm_file)->default_value(""),
     "The output CSM camera file.")
    ("dem", po::value(&opt.dem)->default_value(""),
     "A reference DEM for the site.")
  ;

  general_options.add(vw::GdalWriteOptionsDescription(opt));

  po::options_description positional("");
  po::positional_options_description positional_desc;

  std::string usage("[options]");
  bool allow_unregistered = false;
  std::vector<std::string> unregistered;
  po::variables_map vm =
    asp::check_command_line(argc, argv, opt, general_options, general_options,
                            positional, positional_desc, usage,
                            allow_unregistered, unregistered);

  // The dim file is required
  if (opt.dim_file.empty())
    vw::vw_throw(vw::ArgumentErr() << "Missing the input .DIMA file.\n"
                 << usage << general_options);

  // The output file is required
  if (opt.csm_file.empty())
    vw::vw_throw(vw::ArgumentErr() << "Missing the output CSM file.\n");

  // The CCD must be positive
  if (opt.ccd < 1)
    vw::vw_throw(vw::ArgumentErr() << "The CCD id must be positive.\n");

  // The reference DEM is required
  if (opt.dem.empty())
    vw::vw_throw(vw::ArgumentErr() << "Missing the reference DEM.\n");
    
  // TODO(oalexan1): Add logic to log to file

} // End function handle_arguments

// Given a vector of values and a spacing, check that the values have
// that spacing, with given tolerance.
void checkSpacing(std::vector<double> const& vals, double spacing, double tol,
                  std::string const& tag) {

  // The spacing must be positive
  if (spacing <= 0)
    vw::vw_throw(vw::ArgumentErr() << "Expecting positive time spacing between samples.\n");

  for (size_t i = 1; i < vals.size(); i++) {
    double diff = vals[i] - vals[i-1];
    double err = std::abs(diff - spacing);
    if (err > tol)
      vw::vw_throw(vw::ArgumentErr() << "Expecting all " << tag
        << " values to be spaced by " << spacing << ". Found a discrepancy of "
        << err << " seconds at index " << i << ".\n");
  }

  return;
}

// A function to create a georeference in stereographic coordinates
// at given position with given datum.
void produceStereographicGeoref(vw::Vector3 const& pos,
                                vw::cartography::Datum const& datum,
                                vw::cartography::GeoReference & georef) {

  // Create a georeference at the last position
  vw::Vector3 llh = datum.cartesian_to_geodetic(pos);
  georef.set_datum(datum);
  double scale  = 1.0, false_easting = 0.0, false_northing = 0.0;
  georef.set_stereographic(llh[1], llh[0], scale, false_easting, false_northing);

  return;
}

// Given a set of orbital positions acquired at with uniform time spacing,
// corresponding velocities, the times, the time spacing, extrapolate one more
// position by fitting a parabola. This was shown to given results to within 1
// km. Do this in projected coordinates, where the curvature is less, and the
// error was validated to be half as much. Do this for velocity in ECEF. Add to
// the time by incrementing the last time by the time interval.
void extrapolatePosition(vw::cartography::Datum const& datum,
                         double                        dt,
                         std::vector<double>         & times,
                         std::vector<vw::Vector3>    & positions,
                         std::vector<vw::Vector3>    & velocities) {

  // Must have at least 3 positions
  if (positions.size() < 3)
    vw::vw_throw(vw::ArgumentErr() << "Expecting at least 3 positions for parabola "
                  "extrapolation.\n");

  // The time spacing must be positive
  if (dt <= 0)
    vw::vw_throw(vw::ArgumentErr() << "Expecting positive time spacing between samples.\n");

  // Sanity check for spacing
  double tol = 1e-6;
  checkSpacing(times, dt, tol, "position");

  // Produce a georef at the last position
  vw::cartography::GeoReference georef;
  produceStereographicGeoref(positions.back(), datum, georef);

  // Find projected coordinates
  std::vector<vw::Vector3> projections(positions.size());
  for (size_t i = 0; i < positions.size(); i++)
    projections[i]
      = georef.geodetic_to_point(georef.datum().cartesian_to_geodetic(positions[i]));

  // We do the extrapolation at every position, to be able to check how we are
  // doing before the final extrapolation.
  int num_pos = positions.size(); // the position size will grow, but this will not

  for (int i = 2; i < num_pos; i++) {

    vw::Vector3 u = projections[i-2];
    vw::Vector3 v = projections[i-1];
    vw::Vector3 w = projections[i];

    // Extrapolate by fitting a parabola
    vw::Vector3 next_proj = u - 3 * v + 3 * w;
    vw::Vector3 next_pos
      = georef.datum().geodetic_to_cartesian(georef.point_to_geodetic(next_proj));

    // Do this for velocity as well
    u = velocities[i-2];
    v = velocities[i-1];
    w = velocities[i];
    vw::Vector3 next_vel = u - 3 * v + 3 * w;

    // if we are not at the end, see how this prediction compares
    #if 0
    if (i < num_pos - 1) {
      std::cout << "Error in extrapolation at position " << i << " is = "
        << vw::math::norm_2(positions[i+1] - next_pos) << std::endl;

      std::cout << "Error in extrapolation at velocity " << i << " is = "
        << vw::math::norm_2(velocities[i+1] - next_vel) << std::endl;
    }
    #endif

    if (i == num_pos - 1) {
      // Append the new position, last velocity, and a new time
      positions.push_back(next_pos);
      velocities.push_back(next_vel);
      times.push_back(times.back() + dt);
    }
  }

  return;
}

void createCsmModel(double first_line_time, double dt_line,
                    double t0_ephem, double dt_ephem, double focal_length, 
                    double roll, double pitch, double yaw,
                    std::string              const& view,
                    vw::Vector2              const& optical_center,
                    vw::Vector2              const& image_size,
                    vw::cartography::Datum   const& datum,
                    std::vector<vw::Vector3> const& positions,
                    std::vector<vw::Vector3>const & velocities,
                    std::vector<vw::Vector3> const& rpy,
                    asp::CsmModel& model) {

  // Create a georeference at the last position
  vw::cartography::GeoReference georef;
  produceStereographicGeoref(positions.back(), datum, georef);

  // Assemble the cam2world matrices
  std::vector<vw::Matrix3x3> cam2world(positions.size());
  for (size_t i = 0; i < positions.size(); i++) {

    vw::Vector3 beg_pos = positions[i];
    // Normalized velocity
    vw::Vector3 vel = velocities[i];
    vel = vel / vw::math::norm_2(vel);
    vw::Vector3 end_pos = beg_pos + asp::satSimDelta() * vel;

    vw::Vector3 llh = georef.datum().cartesian_to_geodetic(beg_pos);

    vw::Vector3 beg_proj
      = georef.geodetic_to_point(georef.datum().cartesian_to_geodetic(beg_pos));
    vw::Vector3 end_proj
      = georef.geodetic_to_point(georef.datum().cartesian_to_geodetic(end_pos));

    vw::Vector3 proj_along, proj_across;
    asp::calcProjAlongAcross(beg_proj, end_proj, proj_along, proj_across);

    vw::Vector3 along, across; // ECEF
    asp::calcEcefAlongAcross(georef, asp::satSimDelta(),
                              proj_along, proj_across, beg_proj,
                              along, across);

    vw::Vector3 down = vw::math::cross_prod(along, across);
    down = down / vw::math::norm_2(down);

    // The satellite orientation if perfectly aligned with the trajectory
    vw::Matrix3x3 sat2world;
    asp::assembleCam2WorldMatrix(along, across, down, sat2world);

    // Adjust for the measured roll-pitch-yaw of the satellite from the PRISM data
    vw::Matrix3x3 sat2sat = asp::rollPitchYaw(rpy[i][0], rpy[i][1], rpy[i][2]);

    // Go from satellite orientation to sensor orientation
    vw::Matrix3x3 cam2sat = asp::rollPitchYaw(roll, pitch, yaw);

    // It looks that the PRISM camera is mounted in reverse, so need to use
    // the inverse of the rotation matrix.
    vw::Matrix3x3 cam2cam = vw::math::inverse(asp::rotationXY());

    // Put it all together
    cam2world[i] = sat2world * sat2sat * cam2sat * cam2cam;
  }

  // Form and save the camera
  asp::populateCsmLinescan(first_line_time, dt_line,
                            t0_ephem, dt_ephem,
                            t0_ephem, dt_ephem,  // for poses
                            //t0_quat, dt_quat, // for later
                            focal_length, optical_center, image_size, datum, view,
                            positions, velocities, cam2world, model);
}

// TODO(oalexan1): This needs to be modularized

void prism2asp(Options const& opt) {
  
  int ncols = -1, nrows = -1;
  double first_line_time = -1, last_line_time = -1;
  std::string view;
  std::vector<vw::Vector3> positions, velocities, rpy; // rpy = roll-pitch-yaw
  std::vector<double> position_times, rpy_times;
  asp::parsePrismXml(opt.dim_file, ncols, nrows, view, first_line_time, last_line_time,
                      positions, velocities, position_times, rpy, rpy_times);

  // TODO(oalexan1): Modularize the block below

  // Shift all values closer to the origin to avoid numerical issues with interpolation
  double time_shift = first_line_time;
  first_line_time -= time_shift;
  last_line_time  -= time_shift;
  for (size_t i = 0; i < position_times.size(); i++)
    position_times[i] -= time_shift;
  for (size_t i = 0; i < rpy_times.size(); i++)
    rpy_times[i] -= time_shift;

  // WGS84 datum
  vw::cartography::Datum datum("WGS84");

  // Position sampling
  double t0_ephem = position_times[0];
  double dt_ephem = position_times[1] - position_times[0];

  // The tolerance should not be too small as the times in seconds can be large.
  // A satellite velocity under 10 km / s would result in movement of less
  // than 1e-2 m in 1e-6 seconds.
  double tol = 1e-6;
  checkSpacing(position_times, dt_ephem, tol, "position");

  // Roll-pitch-yaw sampling
  double t0_quat = rpy_times[0];
  double dt_quat = rpy_times[1] - rpy_times[0];
  checkSpacing(rpy_times, dt_quat, tol, "roll-pitch-yaw");

  double dt_line = (last_line_time - first_line_time) / (nrows - 1.0);
  vw::Vector2 image_size(ncols, nrows);

  // This is a fix for the range of the position times not encompassing the
  // range of the image lines times. This is a temporary fix, to be refined later.
  while (position_times.back() < last_line_time - tol)
    extrapolatePosition(datum, dt_ephem, position_times, positions, velocities);

  // Sanity check to ensure interpolation works later
  if (position_times[0] > first_line_time + tol ||
      position_times.back() < last_line_time - tol)
    vw::vw_throw(vw::ArgumentErr()
      << "The position time range must encompass the image lines time range.\n");
  if (rpy_times[0] > first_line_time + tol ||
    rpy_times.back() < last_line_time - tol)
    vw::vw_throw(vw::ArgumentErr()
      << "The roll-pitch-yaw time range must encompass the image lines time range.\n");

  // Optical offset
  // TODO(oalexan1): These need refinement. The [Schneider] doc has better values.
  // Offset for the merged image. Subtract 32 as that is the overlap amount
  // between CCDs. Heuristics for now.
  double roll = 0.0, pitch = 0, yaw = 0.0;
  double offset_factor = 0.0;
  if (view == "PRISM forward") {
    pitch = 23.8;
    offset_factor = 7.3; //4.81; // (latter is before adding rpy)
  } else if (view == "PRISM nadir") {
    pitch = 0.0;
    offset_factor = 5.8; // (latter is before adding rpy)
  } else if (view == "PRISM backward") {
    pitch = -23.8;
    offset_factor = 6.0; // 3.31; // (latter is before adding rpy)
  } else {
    vw::vw_throw(vw::ArgumentErr() << "Expecting forward, nadir or backward view. "
                  << "Got: " << view << ".\n");
  }

  // CCD strip image width
  int w = image_size[0];
  int global_offset = (w-32) * offset_factor; // experimentally found
  // Adapt to current CCD
  int local_offset = global_offset - (opt.ccd - 1) * (w-32);
  vw::Vector2 optical_center(local_offset, 0); // 0 offset in y

  // Focal length
  // TODO(oalexan1): Use honest focal length and optical offset
  // The doc says 1.939 m focal length.
  double ht = 689880; // height above Earth's surface, in meters
  double dx = 2.5; // resolution in meters
  double focal_length = ht / dx; // focal length in pixels

  // Create the CSM model
  asp::CsmModel model;
  createCsmModel(first_line_time, dt_line, t0_ephem, dt_ephem, focal_length,
                 roll, pitch, yaw, view, optical_center, image_size, datum,
                 positions, velocities, rpy, model);
  
  // Read and interpolate the DEM
  vw::ImageViewRef<vw::PixelMask<float>> dem;
  float dem_nodata_val = -std::numeric_limits<float>::max(); // will change
  vw::cartography::GeoReference dem_georef;
  vw::cartography::readGeorefImage(opt.dem, dem_nodata_val, dem_georef, dem);
  vw::PixelMask<float> nodata_pix(0); nodata_pix.invalidate();
  vw::ValueEdgeExtension<vw::PixelMask<float>> nodata_ext(nodata_pix); 
  auto interp_dem = interpolate(dem, vw::BicubicInterpolation(), nodata_ext);

  // Upper-left corner
  double lon = -108.1266445, lat = 39.3974048;
  vw::Vector2 dem_pix = dem_georef.lonlat_to_pixel(vw::Vector2(lon, lat));
  std::cout << "--dem pix is " << dem_pix << std::endl;

  auto dem_ht = interp_dem(dem_pix[0], dem_pix[1]);
  if (!vw::is_valid(dem_ht))
    vw::vw_throw(vw::ArgumentErr() << "Could not interpolate into the DEM.\n");
  
  vw::Vector3 llh(lon, lat, dem_ht.child());
  std::cout << "llh is " << llh << std::endl;
  vw::Vector3 xyz = dem_georef.datum().geodetic_to_cartesian(llh);
  std::cout << "--xyz is " << xyz << std::endl;
  vw::Vector2 cam_pix = model.point_to_pixel(xyz);
  std::cout << "--cam pix is " << cam_pix << std::endl;
  
  // The corner pixel is based on NDUMMY_LEFT of first band
  vw::Vector2 corner_pix(3520, 0);
  std::cout << "--corner pix is " << corner_pix << std::endl;
  
  // vw::Vector3 cam_ctr = camera_model->camera_center(query_pixel);
  // vw::Vector3 cam_dir = camera_model->pixel_to_vector(query_pixel);
  // double height_guess = vw::cartography::demHeightGuess(masked_dem);

  // // Intersect the ray going from the given camera pixel with a DEM
  // // Use xyz_guess as initial guess and overwrite it with the new value
  // bool treat_nodata_as_zero = false;
  // bool has_intersection = false;
  // double dem_height_error_tol = 1e-3; // 1 mm
  // double max_abs_tol = std::min(dem_height_error_tol, 1e-14);
  // double max_rel_tol = max_abs_tol;
  // int num_max_iter = 100;
  // vw::Vector3 xyz_guess(0, 0, 0);
  // vw::Vector3 xyz = vw::cartography::camera_pixel_to_dem_xyz
  //   (cam_ctr, cam_dir, masked_dem,
  //     dem_georef, treat_nodata_as_zero,
  //     has_intersection, dem_height_error_tol, 
  //     max_abs_tol, max_rel_tol, 
  //     num_max_iter, xyz_guess, height_guess);

  vw::vw_out() << "Writing: " << opt.csm_file << "\n";
  model.saveState(opt.csm_file);

}

int main(int argc, char * argv[]) {

  Options opt;
  try {

    handle_arguments(argc, argv, opt);
    prism2asp(opt);

  } ASP_STANDARD_CATCHES;

  return 0;
}
