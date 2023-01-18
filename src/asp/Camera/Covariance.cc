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

// Logic for propagation of covariance through stereo triangulation 

#include <asp/Camera/Covariance.h>
#include <asp/Camera/LinescanDGModel.h>
#include <asp/Core/StereoSettings.h>

#include <vw/Stereo/StereoModel.h>
#include <vw/Math/LinearAlgebra.h>

#include <iostream>

using namespace vw::camera;

namespace asp {

// Change in satellite position (measured in meters) and satellite orientation
// measured in normalized quaternions, to be used for numerical differencing.
// One has to be mindful of the fact that the positions are on the order of
// 7.0e6 meters given the distance from satellite to Earth center in ECEF,
// so the position delta should not be too tiny.
const double deltaPosition = 0.01; // measured in meters
const double deltaQuat     = 1.0e-6; // given that quaternions are normalized

// Given 0 <= num < 15, return a perturbation in satellite position. The
// starting one is the zero perturbation, then perturb first
// coordinate in the positive and then negative direction, then same
// for second and third coordinate. The rest of the perturbations are
// 0 as those indices are used to perturb the quaternions.
// So, return (0, 0, 0), (deltaPosition, 0, 0), (-deltaPosition, 0, 0)
// (0, deltaPosition, 0), (0, -deltaPosition, 0), and so on.
vw::Vector<double, 3> positionDelta(int num) {

  vw::Vector<double, 3> ans; // this is 0
  if (num == 0)
    return ans; // nominal position value, no perturbation
  if (num > 6) 
    return ans; // nominal position value, will perturb the quaternion then

  double sign = 1.0; // sign of the perturbation: 1, -1, 1, -1, etc.
  if (num % 2 == 0) 
    sign = -1.0;

  int index = (num - 1)/2; // get 0, 1, or 2
  ans[index] = sign * deltaPosition;
  
  return ans;
}

// Similar logic as above for the quaternion, for indices 7, ..., 14 (8 of them)
vw::Vector<double, 4> quatDelta(int num) {

  vw::Vector<double, 4> ans; // this is 0
  if (num <= 6) 
    return ans; // nominal quat value, will perturb the positions then

  num = num - 6; // now num = 1, 2, ... 8
  if (num > 8) 
    vw::vw_throw(vw::ArgumentErr() << "Out of bounds in quatDelta().\n");
  
  double sign = 1.0; // sign of the perturbation: 1, -1, 1, -1, etc.
  if (num % 2 == 0) 
    sign = -1.0;

  int index = (num - 1)/2; // get 0, 1, 2, or 3
  ans[index] = sign * deltaQuat;
  
  return ans;
}

// Number of nominal and perturbed cameras when the covariance is computed.
int numCamsForCovariance() {
  // One nominal camera. Then one positive and negative perturbation
  // for each position (3) and quaternion (4).
  return 15; 
}

// See the .h file for the description
void scaledTriangulationJacobian(vw::camera::CameraModel const* cam1,
                                 vw::camera::CameraModel const* cam2,
                                 vw::Vector2 const& pix1,
                                 vw::Vector2 const& pix2,
                                 vw::Matrix<double> & J) {

  // Handle adjusted cameras
  bool adjusted_cameras = false;
  const AdjustedCameraModel *adj_cam1 = dynamic_cast<const AdjustedCameraModel*>(cam1);
  const AdjustedCameraModel *adj_cam2 = dynamic_cast<const AdjustedCameraModel*>(cam2);
  if ((adj_cam1 == NULL) != (adj_cam2 == NULL))
    vw::vw_throw(vw::ArgumentErr() << "The cameras must be either both "
                 << "adjusted or both unadjusted.\n");

  vw::Matrix3x3 cam1_rot, cam2_rot;
  vw::Vector3 cam1_shift, cam2_shift;
  if (adj_cam1 != NULL && adj_cam2 != NULL) {
    adjusted_cameras = true;
    // transforms from unadjusted to adjusted coordinates
    vw::Matrix4x4 cam1_adj = adj_cam1->ecef_transform();
    vw::Matrix4x4 cam2_adj = adj_cam2->ecef_transform();
    cam1_rot = submatrix(cam1_adj, 0, 0, 3, 3);
    cam2_rot = submatrix(cam2_adj, 0, 0, 3, 3);
    cam1_shift = vw::Vector3(cam1_adj(0, 3), cam1_adj(1, 3), cam1_adj(2, 3));
    cam2_shift = vw::Vector3(cam2_adj(0, 3), cam2_adj(1, 3), cam2_adj(2, 3));
  }
  
  DGCameraModel const* dg_cam1 = dynamic_cast<DGCameraModel const*>(unadjusted_model(cam1));
  DGCameraModel const* dg_cam2 = dynamic_cast<DGCameraModel const*>(unadjusted_model(cam2));
  if (dg_cam1 == NULL || dg_cam2 == NULL) 
    vw::vw_throw(vw::ArgumentErr() << "Expecting DG cameras.\n");

  // Numerical differences will be used. Camera models with deltaPosition and deltaQuat
  // perturbations have already been created in LinescanDGModel.cc using the positionDelta()
  // and quatDelta() functions from above.
  if (dg_cam1->m_perturbed_cams.empty() || dg_cam2->m_perturbed_cams.empty()) 
    vw::vw_throw(vw::ArgumentErr() << "The perturbed cameras were not set up.\n");
  
  if (dg_cam1->m_perturbed_cams.size() != dg_cam2->m_perturbed_cams.size())
    vw::vw_throw(vw::ArgumentErr()
                 << "The number of perturbations in the two cameras do not agree.\n");

  // Find the camera center and direction for first unperturbed
  // camera, and for the perturbed versions. Same for the second
  // camera.
  std::vector<vw::Vector3> cam1_dirs, cam1_ctrs, cam2_dirs, cam2_ctrs;
  cam1_dirs.push_back(dg_cam1->pixel_to_vector(pix1));
  cam1_ctrs.push_back(dg_cam1->camera_center(pix1));
  cam2_dirs.push_back(dg_cam2->pixel_to_vector(pix2));
  cam2_ctrs.push_back(dg_cam2->camera_center(pix2));
  for (size_t it = 0; it < dg_cam1->m_perturbed_cams.size(); it++) {
    cam1_dirs.push_back(dg_cam1->m_perturbed_cams[it]->pixel_to_vector(pix1));
    cam1_ctrs.push_back(dg_cam1->m_perturbed_cams[it]->camera_center(pix1));
    cam2_dirs.push_back(dg_cam2->m_perturbed_cams[it]->pixel_to_vector(pix2));
    cam2_ctrs.push_back(dg_cam2->m_perturbed_cams[it]->camera_center(pix2));
  }

  // Apply adjustments
  if (adjusted_cameras) {
    for (size_t it = 0; it < cam1_dirs.size(); it++) {
      cam1_dirs[it] = cam1_rot * cam1_dirs[it];
      cam2_dirs[it] = cam2_rot * cam2_dirs[it];
      cam1_ctrs[it] = cam1_rot * cam1_ctrs[it] + cam1_shift;
      cam2_ctrs[it] = cam2_rot * cam2_ctrs[it] + cam2_shift;
    }
  }
  
  // Nominal triangulation point
  vw::Vector3 tri_nominal, err_nominal;
  // If triangulation fails, it can return NaN
  tri_nominal
    = vw::stereo::triangulate_pair(cam1_dirs[0], cam1_ctrs[0], cam2_dirs[0], cam2_ctrs[0],
                                   err_nominal);
  if (tri_nominal != tri_nominal) // NaN
    vw::vw_throw(vw::ArgumentErr() << "Could not triangulate.\n");

  // The matrix to go from the NED coordinate system to ECEF
  vw::cartography::Datum const& datum = dg_cam1->datum; // alias
  vw::Vector3 llh = datum.cartesian_to_geodetic(tri_nominal);
  vw::Matrix3x3 NedToEcef = datum.lonlat_to_ned_matrix(subvector(llh, 0, 2));
  vw::Matrix3x3 EcefToNed = inverse(NedToEcef);

  // There are 14 input variables: 3 positions and 4 quaternions for
  // cam1, and same for cam2. For each of them must compute a centered
  // difference. The output has 3 variables. As documented in the
  // .h file for this function, the vector from the nominal to perturbed
  // triangulated point will be converted to North-East-Down
  // coordinates at the nominal triangulated point.
  J.set_size(3, 14);
  J.set_zero();
  for (int coord = 0; coord < 14; coord++) {

    vw::Vector3 cam1_dir_plus, cam1_ctr_plus, cam2_dir_plus, cam2_ctr_plus;
    vw::Vector3 cam1_dir_minus, cam1_ctr_minus, cam2_dir_minus, cam2_ctr_minus;
    if (coord < 7) {
      // The perturbed cameras store positive and negative
      // perturbations, in alternating order. See positionDelta() and
      // quatDelta() for the book-keeping. Note that a perturbation in
      // the satellite quaternion also affects the camera center,
      // given how one converts from satellite to camera coordinates
      // when the DG model is created.

      // Since at position 0 in cam_dirs we store the nominal (unperturbed)
      // values, add 1 below.
      cam1_dir_plus  = cam1_dirs[2*coord + 1]; cam1_ctr_plus  = cam1_ctrs[2*coord + 1];
      cam1_dir_minus = cam1_dirs[2*coord + 2]; cam1_ctr_minus = cam1_ctrs[2*coord + 2];
    } else {
      // When variables affecting second camera change, the first one stays at nominal value.
      cam1_dir_plus  = cam1_dirs[0]; cam1_ctr_plus  = cam1_ctrs[0];
      cam1_dir_minus = cam1_dirs[0]; cam1_ctr_minus = cam1_ctrs[0];
    }

    // For the second camera, the book-keeping is reversed
    if (coord < 7) {
      // Second camera values do not change when first camera inputs change
      cam2_dir_plus  = cam2_dirs[0]; cam2_ctr_plus  = cam2_ctrs[0];
      cam2_dir_minus = cam2_dirs[0]; cam2_ctr_minus = cam2_ctrs[0];
    } else {
      int coord2 = coord - 7; // has values 0, 1, ..., 6
      cam2_dir_plus  = cam2_dirs[2*coord2 + 1]; cam2_ctr_plus  = cam2_ctrs[2*coord2 + 1];
      cam2_dir_minus = cam2_dirs[2*coord2 + 2]; cam2_ctr_minus = cam2_ctrs[2*coord2 + 2];
    }

    vw::Vector3 tri_plus, err_plus, tri_minus, err_minus;
    tri_plus = vw::stereo::triangulate_pair(cam1_dir_plus, cam1_ctr_plus,
                                            cam2_dir_plus, cam2_ctr_plus, err_plus);
    tri_minus = vw::stereo::triangulate_pair(cam1_dir_minus, cam1_ctr_minus,
                                             cam2_dir_minus, cam2_ctr_minus, err_minus);

    // Find the triangulated points in the local NED (horizontal-vertical)
    // coordinate system.
    vw::Vector3 ned_plus = EcefToNed * (tri_plus - tri_nominal);
    vw::Vector3 ned_minus = EcefToNed * (tri_minus - tri_nominal);

    // Find the numerical partial derivative, but do not divide by the
    // spacing (deltaPosition or deltaQuat) as that makes the numbers
    // huge. We will compensate for when use use this Jacobian to
    // propagate the satellite position and quaternion covariances
    // (matrix SC), by the formula J * SC * J^T. Then, we will divide SC
    // by these squared delta quantities, which is the right thing to
    // do, because the values in SC are tiny, and, in fact, on the
    // order of the squares of the delta values.
    vw::Vector3 ned_diff = (ned_plus - ned_minus)/2.0;
    
    for (int row = 0; row < 3; row++) 
      J(row, coord) = ned_diff[row];
  }

  return;
}

// Given upper-right values in a symmetric matrix of given size, find
// the lower-left values by reflection, and insert them as a block
// starting at the desired row and column. Used to populate the joint
// covariance matrix. Per DigitalGlobe's doc, the covariances are
// stored as c11, c12, c13, ..., c22, c23, ...
void insertBlock(int start, int size, double* inputVals, vw::Matrix<double> & C) {
  int count = 0;
  for (int row = 0; row < size; row++) {
    for (int col = row; col < size; col++) {
      C(start + row, start + col) = inputVals[count];
      C(start + col, start + row) = inputVals[count];
      count++;
    }
  }
}
  
// See the .h file for the description
void scaledSatelliteCovariance(vw::camera::CameraModel const* cam1,
                               vw::camera::CameraModel const* cam2,
                               vw::Vector2 const& pix1,
                               vw::Vector2 const& pix2,
                               vw::Matrix<double> & C) {

  // Initialize the output
  // 3 positions for cam 1, 4 orientations for cam1, 3 positions for cam2, 4 orientations
  // for cam2. So, four blocks in total. The resulting matrix must be symmetric.
  C.set_size(14, 14);
  C.set_zero();

  // Here it is not important that the camera are adjusted or not, as all that is needed
  // are the input covariances.
  DGCameraModel const* dg_cam1 = dynamic_cast<DGCameraModel const*>(unadjusted_model(cam1));
  DGCameraModel const* dg_cam2 = dynamic_cast<DGCameraModel const*>(unadjusted_model(cam2));
  if (dg_cam1 == NULL || dg_cam2 == NULL) 
    vw::vw_throw(vw::ArgumentErr() << "Expecting DG cameras.\n");

  // Find the covariances at given pixel by interpolation in the input table.
  // Use nearest neighbor interpolation as covariances are known with
  // just a few digits of precision and are not meant to be smooth.
  double p_cov1[SAT_POS_COV_SIZE], p_cov2[SAT_POS_COV_SIZE];
  double q_cov1[SAT_QUAT_COV_SIZE], q_cov2[SAT_QUAT_COV_SIZE];  
  dg_cam1->interpSatellitePosCov(pix1, p_cov1);
  dg_cam1->interpSatelliteQuatCov(pix1, q_cov1);
  dg_cam2->interpSatellitePosCov(pix2, p_cov2);
  dg_cam2->interpSatelliteQuatCov(pix2, q_cov2);

  // This is useful for seeing which input covariance has a bigger effect.
  // The default value of these factors is 1.
  double pf = asp::stereo_settings().position_covariance_factor;
  double qf = asp::stereo_settings().orientation_covariance_factor;
  
  // Scale these per scaledTriangulationJacobian().
  for (int ip = 0; ip < SAT_POS_COV_SIZE; ip++) {
    p_cov1[ip] = pf * p_cov1[ip] / (deltaPosition * deltaPosition); 
    p_cov2[ip] = pf * p_cov2[ip] / (deltaPosition * deltaPosition); 
  }
  for (int iq = 0; iq < SAT_QUAT_COV_SIZE; iq++) {
    q_cov1[iq] = qf * q_cov1[iq] / (deltaQuat * deltaQuat); 
    q_cov2[iq] = qf * q_cov2[iq] / (deltaQuat * deltaQuat); 
  }

  // Put these in the covariance matrix
  insertBlock(0,  3, p_cov1, C);
  insertBlock(3,  4, q_cov1, C);
  insertBlock(7,  3, p_cov2, C);
  insertBlock(10, 4, q_cov2, C);

#if 0
  std::cout << "Produced matrix " << std::endl;
  for (int row = 0; row < 14; row++) {
    for (int col = 0; col < 14; col++) {
      std::cout << C(row, col) << " ";
    }
    std::cout << std::endl;
  }

  // Debug code. This shows that some quaternion covariances have a
  // negative determinant. That is because an eigenvalue is very close
  // to 0 or even negative (but small). This singularity goes away
  // after the covariances are propagated.
  std::cout << "determinant1 " << det(submatrix(C, 0, 0, 3, 3)) << std::endl;
  std::cout << "determinant2 " << det(submatrix(C, 3, 3, 4, 4)) << std::endl;
  std::cout << "determinant3 " << det(submatrix(C, 7, 7, 3, 3)) << std::endl;
  std::cout << "determinant4 " << det(submatrix(C, 10, 10, 4, 4)) << std::endl;
#endif
  
  return;
}

// See .h for the doc.
vw::Vector2 propagateCovariance(vw::camera::CameraModel const* cam1,
                                vw::camera::CameraModel const* cam2,
                                vw::Vector2 const& pix1,
                                vw::Vector2 const& pix2) {

  // The Jacobian of the transform from ephemeris and attitude to the triangulated
  // point in NED coordinates, multiplied by a scale factor.
  vw::Matrix<double> J;
  asp::scaledTriangulationJacobian(cam1, cam2, pix1, pix2, J);

  // The input covariance, divided by the square of the above scale factor.
  vw::Matrix<double> C;
  asp::scaledSatelliteCovariance(cam1, cam2, pix1, pix2, C);

  // Propagate the covariance
  // Per: https://en.wikipedia.org/wiki/Propagation_of_uncertainty#Non-linear_combinations
  vw::Matrix<double> JT = transpose(J);
  vw::Matrix<double> P = J * C * JT;

#if 0
  // Useful debug code
  std::cout << "NED covariance " << P << std::endl;
  vw::Vector<std::complex<double>> e;
  vw::math::eigen(P, e);
  std::cout << "Eigenvalues: " << e << std::endl;
#endif
  
  // Horizontal component is the square root of the determinant of the
  // upper-left 2x2 block (horizontal plane component), which is the
  // same as the square root of the product of eigenvalues of this
  // matrix.  Intuitively, the area of an ellipse is the product of
  // semi-axes, which is the product of eigenvalues. Then, a circle
  // with radius which is the square root of the product of semi-axes
  // has the same area.
  vw::Matrix2x2 H = submatrix(P, 0, 0, 2, 2);
  vw::Vector2 ans;
  ans[0] = sqrt(det(H));

  // Vertical component is the z variance
  ans[1] = P(2, 2);

  // Check for NaN. Then the caller will return the zero vector, which
  // signifies that the there is no valid data
  if (ans != ans) 
    vw::vw_throw(vw::ArgumentErr() << "Could not compute the covariance.\n");
  
  return ans;
}
  
} // end namespace asp