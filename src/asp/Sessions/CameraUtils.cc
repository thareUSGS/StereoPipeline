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

/// Camera utilities that need the stereo session

/// \file CameraUtils.cc

#include <asp/Sessions/CameraUtils.h>
#include <asp/Sessions/StereoSessionFactory.h>

#include <vw/Core/Exception.h>
#include <vw/Camera/PinholeModel.h>
#include <vw/Camera/CameraUtilities.h>
#include <vw/InterestPoint/InterestData.h>

#include <string>
#include <iostream>


using namespace vw;

namespace asp {

void load_camera(std::string const& image_file,
                 std::string const& camera_file,
                 std::string const& out_prefix,
                 vw::GdalWriteOptions const& opt,
                 bool approximate_pinhole_intrinsics,
                 // Outputs
                 std::string & stereo_session,
                 vw::CamPtr  & camera_model,
                 bool        & single_threaded_camera) {

  // TODO(oalexan1): Replace this with a simpler camera model loader class.
  // But note that this call also refines the stereo session name.
  std::string input_dem = ""; // No DEM
  bool allow_map_promote = false;
  // TODO(oalexan1): The quiet flag should likely be false just once.
  bool quiet = false; 
  asp::SessionPtr session
    (asp::StereoSessionFactory::create(stereo_session, opt,
                                        image_file, image_file,
                                        camera_file, camera_file,
                                        out_prefix, input_dem,
                                        allow_map_promote, quiet));
  camera_model = session->camera_model(image_file, camera_file);
  
  // This is necessary to avoid a crash with ISIS cameras which is single-threaded
  // TODO(oalexan1): Check this value for csm cameras embedded in ISIS images.
  single_threaded_camera = (!session->supports_multi_threading());
  
  if (approximate_pinhole_intrinsics) {
    boost::shared_ptr<vw::camera::PinholeModel> pinhole_ptr = 
      boost::dynamic_pointer_cast<vw::camera::PinholeModel>(camera_model);
    // Replace lens distortion with fast approximation
    vw::camera::update_pinhole_for_fast_point2pixel<vw::camera::TsaiLensDistortion>
      (*(pinhole_ptr.get()), file_image_size(image_file));
  }

}

// Load cameras from given image and camera files
void load_cameras(std::vector<std::string> const& image_files,
                  std::vector<std::string> const& camera_files,
                  std::string const& out_prefix, 
                  vw::GdalWriteOptions const& opt,
                  bool approximate_pinhole_intrinsics,
                  // Outputs
                  std::string & stereo_session, // may change
                  bool & single_threaded_camera,
                  std::vector<boost::shared_ptr<vw::camera::CameraModel>> & camera_models) {

  // Sanity check
  if (image_files.size() != camera_files.size()) 
    vw_throw(ArgumentErr() << "Expecting as many images as cameras.\n");  

  // Initialize the outputs
  camera_models.resize(image_files.size());
  single_threaded_camera = false; // may change
  
  // TODO(oalexan1): Must load one camera first. Then, if the result is that it
  // is not single-threaded, load the rest in parallel. This can greatly speed
  // up loading of many cameras.
  for (size_t i = 0; i < image_files.size(); i++) {
    // Use local variables to for thread-safety
    bool local_single_threaded_camera = false;
    std::string local_stereo_session = stereo_session; 
    load_camera(image_files[i], camera_files[i], out_prefix, opt,
                approximate_pinhole_intrinsics,
                local_stereo_session, camera_models[i], local_single_threaded_camera);

    // Update these based on the camera just loaded
    // TODO(oalexan1): Must use here a lock when using multiple threads.
    single_threaded_camera = local_single_threaded_camera;
    stereo_session = local_stereo_session;
  }
  
  return;
}

// Find the datum based on cameras. Return true on success. Otherwise don't set it.
// TODO(oalexan1): Pass to this only the first camera and image file.
bool datum_from_camera(std::string const& image_file,
                        std::string const& camera_file, 
                        std::string & stereo_session, // may change
                        asp::SessionPtr & session, // may be null on input
                        // Outputs
                        vw::cartography::Datum & datum) {
  
  std::string out_prefix = "run";

  // Look for a non-pinole camera, as a pinhole camera does not have a datum
  bool success = false;
  double cam_center_radius = 0.0;
  if (session.get() == NULL) {
    // If a session was not passed in, create it here.
    std::string input_dem = ""; // No DEM
    bool allow_map_promote = false, quiet = true;
    session.reset(asp::StereoSessionFactory::create(stereo_session, // may change
                                                vw::GdalWriteOptions(),
                                                image_file, image_file,
                                                camera_file, camera_file,
                                                out_prefix, input_dem,
                                                allow_map_promote, quiet));
  }
    
  auto cam = session->camera_model(image_file, camera_file);
  cam_center_radius = norm_2(cam->camera_center(vw::Vector2()));
    
  // Pinhole and nadirpinhole cameras do not have a datum
  if (stereo_session != "pinhole" && stereo_session != "nadirpinhole") {
    bool use_sphere_for_non_earth = true;
    datum = session->get_datum(cam.get(), use_sphere_for_non_earth);
    success = true;
    return success; // found the datum
  }
  
  // Guess the based on camera position. Usually one arrives here for pinhole
  // cameras.

  // Datums for Earth, Mars, and Moon
  vw::cartography::Datum earth("WGS84");
  vw::cartography::Datum mars("D_MARS");
  vw::cartography::Datum moon("D_MOON");
  
  double km = 1000.0;
  if (cam_center_radius > earth.semi_major_axis() - 100*km && 
      cam_center_radius < earth.semi_major_axis() + 5000*km) {
    datum = earth;
    success = true;
  } else if (cam_center_radius > mars.semi_major_axis() - 100*km && 
             cam_center_radius < mars.semi_major_axis() + 1500*km) {
    datum = mars;
    success = true;
  } else if (cam_center_radius > moon.semi_major_axis() - 100*km && 
             cam_center_radius < moon.semi_major_axis() + 1000*km) {
    datum = moon;
    success = true;
  }
  
  if (success)
    vw_out() << "Guessed the datum from camera position.\n";

  return success;
}

} // end namespace asp
