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


/// \file bundle_adjust.cc
///

#include <asp/Core/Macros.h>
#include <asp/Tools/bundle_adjust.h>
#include <asp/Sessions/StereoSession.h>
#include <ceres/ceres.h>
#include <ceres/loss_function.h>
namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace vw;
using namespace vw::camera;
using namespace vw::ba;

#if defined(ASP_HAVE_PKG_ISISIO) && ASP_HAVE_PKG_ISISIO == 1
#include <asp/IsisIO/DiskImageResourceIsis.h>
#endif

// Given a vector of strings, identify and store separately the the
// list of GCPs. This should be useful for those programs who accept
// their data in a mass input vector.
std::vector<std::string>
sort_out_gcps( std::vector<std::string>& image_files ) {
  std::vector<std::string> gcp_files;
  std::vector<std::string>::iterator it = image_files.begin();
  while ( it != image_files.end() ) {
    if ( boost::iends_with(boost::to_lower_copy(*it), ".gcp") ){
      gcp_files.push_back( *it );
      it = image_files.erase( it );
    } else
      it++;
  }
  
  return gcp_files;
}

// Given a vector of strings, identify and store separately the list
// of camera models.
std::vector<std::string>
sort_out_cameras( std::vector<std::string>& image_files ) {
  std::vector<std::string> cam_files;
  std::vector<std::string>::iterator it = image_files.begin();
  while ( it != image_files.end() ) {
    if (asp::has_cam_extension( *it ) ||
        boost::iends_with(boost::to_lower_copy(*it), ".xml")
        ){
      cam_files.push_back( *it );
      it = image_files.erase( it );
    } else
      it++;
  }
  
  return cam_files;
}

struct Options : public asp::BaseOptions {
  std::vector<std::string> image_files, camera_files, gcp_files;
  std::string cnet_file, out_prefix, stereo_session_string, cost_function, ba_type;
  
  double lambda, robust_threshold;
  int report_level, min_matches, max_iterations;

  bool save_iteration;
  std::string datum_str;
  double semi_major, semi_minor;

  boost::shared_ptr<ControlNetwork> cnet;
  std::vector<boost::shared_ptr<CameraModel> > camera_models;
  cartography::Datum datum;

  // Make sure all values are initialized, even though they will be over-written later.
  Options():lambda(-1.0), robust_threshold(0), report_level(0), min_matches(0),
            max_iterations(0), save_iteration(false), semi_major(0), semi_minor(0){}
};

// A ceres cost function. Templated by the BundleAdjust model. We pass
// in the observation, the model, and the current camera and point
// indices. The result is the residual, the difference in the
// observation and the projection of the point into the camera,
// normalized by pixel_sigma.
template<class ModelT>
struct BaReprojectionError {
  BaReprojectionError(Vector2 const& observation, Vector2 const& pixel_sigma,
                      ModelT * const ba_model, size_t icam, size_t ipt):
    m_observation(observation),
    m_pixel_sigma(pixel_sigma),
    m_ba_model(ba_model),
    m_icam(icam), m_ipt(ipt){}
  
  template <typename T>
  bool operator()(const T* const camera,
                  const T* const point,
                  T* residuals) const {

    try{
      size_t num_cameras = m_ba_model->num_cameras();
      size_t num_points  = m_ba_model->num_points();
      VW_ASSERT(m_icam < num_cameras,
                ArgumentErr() << "Out of bounds in the number of cameras");
      VW_ASSERT(m_ipt < num_points,
                ArgumentErr() << "Out of bounds in the number of points");

      // Copy the input data to structures expected by the BA model
      typename ModelT::camera_vector_t camera_vec;
      typename ModelT::point_vector_t  point_vec;
      for (size_t c = 0; c < camera_vec.size(); c++)
        camera_vec[c] = (double)camera[c];
      for (size_t p = 0; p < point_vec.size(); p++)
        point_vec[p]  = (double)point[p];

      // Project the current point into the current camera
      Vector2 prediction = (*m_ba_model)(m_ipt, m_icam, camera_vec, point_vec);
      
      // The error is the difference between the predicted and observed position,
      // normalized by sigma.
      residuals[0] = (prediction[0] - m_observation[0])/m_pixel_sigma[0];
      residuals[1] = (prediction[1] - m_observation[1])/m_pixel_sigma[1];

    } catch (const camera::PixelToRayErr& e) {
      // Failed to project into the camera
      residuals[0] = T(1e+20);
      residuals[1] = T(1e+20);
      return false;
    }
    
    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(Vector2 const& observation,
                                     Vector2 const& pixel_sigma,
                                     ModelT * const ba_model,
                                     size_t icam, // camera index
                                     size_t ipt // point index
                                     ){
    return (new ceres::NumericDiffCostFunction<BaReprojectionError, ceres::CENTRAL, 2, ModelT::camera_params_n, ModelT::point_params_n>(new BaReprojectionError(observation, pixel_sigma, ba_model, icam, ipt)));
    
  }
  
  Vector2 m_observation;
  Vector2 m_pixel_sigma;
  ModelT * const m_ba_model;
  size_t m_icam, m_ipt;
};

// A ceres cost function. The residual is the difference between the
// observed 3D point and the current (floating) 3D point, normalized by
// xyz_sigma. Used only for ground control points.
struct XYZError {
  XYZError(Vector3 const& observation, Vector3 const& xyz_sigma):
    m_observation(observation), m_xyz_sigma(xyz_sigma){}
  
  template <typename T>
  bool operator()(const T* const point, T* residuals) const {
    for (size_t p = 0; p < m_observation.size(); p++)
      residuals[p] = ((double)point[p] - m_observation[p])/m_xyz_sigma[p];
    
    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(Vector3 const& observation,
                                     Vector3 const& xyz_sigma){
    return (new ceres::NumericDiffCostFunction<XYZError, ceres::CENTRAL, 3, 3>(new XYZError(observation, xyz_sigma)));
    
  }
  
  Vector3 m_observation;
  Vector3 m_xyz_sigma;
};

ceres::LossFunction* get_loss_function(Options const& opt ){
  double th = opt.robust_threshold;
  ceres::LossFunction* loss_function;
  if      ( opt.cost_function == "l2"     )
    loss_function = NULL;
  else if ( opt.cost_function == "huber"  )
    loss_function = new ceres::HuberLoss(th);
  else if ( opt.cost_function == "cauchy" )
    loss_function = new ceres::CauchyLoss(th);
  else if ( opt.cost_function == "l1"     )
    loss_function = new ceres::SoftLOneLoss(th);
  else{
    vw_throw( ArgumentErr() << "Unknown cost function: " << opt.cost_function
              << " used with solver: " << opt.ba_type << ".\n" );
  }
  return loss_function;
}

// Use Ceres to do bundle adjustment. The camera and point variables
// are stored in arrays.  The projection of point into camera is
// accomplished by interfacing with the bundle adjustment model. In
// the future this class can be bypassed.
template <class ModelT>
void do_ba_ceres(ModelT & ba_model, Options const& opt ){

  ControlNetwork & cnet = *(ba_model.control_network().get());
  CameraRelationNetwork<JFeature> crn;
  crn.read_controlnetwork(cnet);

  size_t num_camera_params = ModelT::camera_params_n;
  size_t num_point_params  = ModelT::point_params_n;
  size_t num_cameras       = ba_model.num_cameras();
  size_t num_points        = ba_model.num_points();

  // The camera adjustment and point variables concatenated into
  // vectors. The camera adjustments start as 0. The points come from
  // the network.
  std::vector<double> cameras_vec(num_cameras*num_camera_params, 0.0);
  double* cameras = &cameras_vec[0];
  std::vector<double> points_vec(num_points*num_point_params, 0.0);
  for (size_t ipt = 0; ipt < num_points; ipt++){
    for (size_t q = 0; q < num_point_params; q++){
      points_vec[ipt*num_point_params + q] = cnet[ipt].position()[q];
    }
  }
  double* points = &points_vec[0];

  // This copy of the points will not change during optimization,
  // they are the observations.
  std::vector<double> points_obs = points_vec;
  
  ceres::Problem problem;
  
  // Add the cost function component for difference of pixel observations
  typedef CameraNode<JFeature>::iterator crn_iter;
  for ( size_t icam = 0; icam < crn.size(); icam++ ) {
    for ( crn_iter fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++ ) {

      // The index of the 3D point
      size_t ipt = (**fiter).m_point_id; 

      VW_ASSERT(icam < num_cameras,
                ArgumentErr() << "Out of bounds in the number of cameras");
      VW_ASSERT(ipt < num_points,
                ArgumentErr() << "Out of bounds in the number of points");

      // The observed value for the projection of point with index ipt into
      // the camera with index icam.
      Vector2 observation = (**fiter).m_location;
      Vector2 pixel_sigma = (**fiter).m_scale;
      
      ceres::CostFunction* cost_function =
        BaReprojectionError<ModelT>::Create(observation, pixel_sigma,
                                            &ba_model, icam, ipt);

      ceres::LossFunction* loss_function = get_loss_function(opt);

      // Each observation corresponds to a pair of a camera and a point
      // which are identified by indices icam and ipt respectively.
      double * camera = cameras + num_camera_params * icam;
      double * point  = points  + num_point_params  * ipt;
      problem.AddResidualBlock(cost_function, loss_function, camera, point);
      
    }
  }

  // Add ground control points
  for (size_t ipt = 0; ipt < num_points; ipt++){
    if (cnet[ipt].type() != ControlPoint::GroundControlPoint) continue;

    Vector3 observation = cnet[ipt].position();
    Vector3 xyz_sigma = cnet[ipt].sigma();

    ceres::CostFunction* cost_function =
      XYZError::Create(observation, xyz_sigma);
    
    ceres::LossFunction* loss_function = get_loss_function(opt);
    
    double * point  = points  + num_point_params  * ipt;
    problem.AddResidualBlock(cost_function, loss_function, point);
  }
  
  // Solve the problem
  ceres::Solver::Options options;
  options.gradient_tolerance = 1e-16;
  options.function_tolerance = 1e-16;

  options.max_num_iterations = opt.max_iterations;
  options.minimizer_progress_to_stdout = (opt.report_level >= vw::ba::ReportFile);
  options.num_threads = 1; //FLAGS_num_threads;
  options.linear_solver_type = ceres::SPARSE_SCHUR;
  //options.ordering_type = ceres::SCHUR;
  //options.eta = 1e-3; // FLAGS_eta;
  //options->max_solver_time_in_seconds = FLAGS_max_solver_time;
  //options->use_nonmonotonic_steps = FLAGS_nonmonotonic_steps;
  //if (FLAGS_line_search) {
  //  options->minimizer_type = ceres::LINE_SEARCH;
  //}

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);
  vw_out() << summary.FullReport() << "\n";
  if (summary.termination_type == ceres::NO_CONVERGENCE){
    // Print a clarifying message, so the user does not think that
    // the algorithm failed. 
    vw_out() << "Found a valid solution, but did not reach the actual minimum."
             << std::endl;
  }
  
  // Save the camera info back to the ba_model so we can save it to disk later
  for (size_t i = 0; i < num_cameras; i++){
    typename ModelT::camera_vector_t cam;
    for (size_t c = 0; c < cam.size(); c++)
      cam[c] = cameras_vec[i*num_camera_params + c];
    ba_model.set_A_parameters(i, cam);
  }
  
}

template <class AdjusterT>
void do_ba(typename AdjusterT::model_type & ba_model,
           typename AdjusterT::cost_type const& cost_function,
           Options const& opt) {

  AdjusterT bundle_adjuster(ba_model, cost_function, false, false);
  
  if ( opt.lambda > 0 )
    bundle_adjuster.set_lambda( opt.lambda );

  std::string iterCameraFile = opt.out_prefix + "iterCameraParam.txt";
  std::string iterPointsFile = opt.out_prefix + "iterPointsParam.txt";
  
  //Clearing the monitoring text files to be used for saving camera params
  if (opt.save_iteration){
    fs::remove(iterCameraFile);
    fs::remove(iterPointsFile);

    // Write the starting locations
    std::cout << "Writing: " << iterCameraFile << std::endl;
    std::cout << "Writing: " << iterPointsFile << std::endl;
    ba_model.bundlevis_cameras_append(iterCameraFile);
    ba_model.bundlevis_points_append(iterPointsFile);
  }

  BundleAdjustReport<AdjusterT >
    reporter( "Bundle Adjust", ba_model, bundle_adjuster,
              opt.report_level );

  double abs_tol = 1e10, rel_tol=1e10;
  double overall_delta = 2;
  int no_improvement_count = 0;
  while ( true ) {
    // Determine if it is time to quit
    if ( bundle_adjuster.iterations() >= opt.max_iterations ) {
      reporter() << "Triggered 'Max Iterations'\n";
      break;
    } else if ( abs_tol < 0.01 ) {
      reporter() << "Triggered 'Abs Tol " << abs_tol << " < 0.01'\n";
      break;
    } else if ( rel_tol < 1e-6 ) {
      reporter() << "Triggered 'Rel Tol " << rel_tol << " < 1e-10'\n";
      break;
    } else if ( no_improvement_count > 4 ) {
      reporter() << "Triggered break, unable to improve after "
                 << no_improvement_count << " iterations\n";
      break;
    }

    overall_delta = bundle_adjuster.update( abs_tol, rel_tol );
    reporter.loop_tie_in();

    // Writing Current Camera Parameters to file for later reading
    if (opt.save_iteration) {
      ba_model.bundlevis_cameras_append(iterCameraFile);
      ba_model.bundlevis_points_append(iterPointsFile);
    }
    if ( overall_delta == 0 )
      no_improvement_count++;
    else
      no_improvement_count = 0;
  }
  reporter.end_tie_in();

}

// Use given cost function. Switch based on solver.
template<class ModelType, class CostFunType>
void do_ba_costfun(CostFunType const& cost_fun, Options const& opt){

  ModelType ba_model(opt.camera_models, opt.cnet);
  
  if ( opt.ba_type == "ceres" ) {
    do_ba_ceres<ModelType>(ba_model, opt);
  } else if ( opt.ba_type == "robustsparse" ) {
    do_ba<AdjustRobustSparse< ModelType,CostFunType> >(ba_model, cost_fun, opt);
  } else if ( opt.ba_type == "robustref" ) {
    do_ba<AdjustRobustRef< ModelType,CostFunType> >(ba_model, cost_fun, opt);
  } else if ( opt.ba_type == "sparse" ) {
    do_ba<AdjustSparse< ModelType, CostFunType > >(ba_model, cost_fun, opt);
  }else if ( opt.ba_type == "ref" ) {
    do_ba<AdjustRef< ModelType, CostFunType > >(ba_model, cost_fun, opt);
  }

  // Save the models to disk
  for (unsigned i=0; i < ba_model.num_cameras(); ++i){
    std::string adjust_file = asp::bundle_adjust_file_name(opt.out_prefix,
                                                           opt.image_files[i]);
    vw_out() << "Writing: " << adjust_file << std::endl;
    ba_model.write_adjustment(i, adjust_file);
  }
  
}

void handle_arguments( int argc, char *argv[], Options& opt ) {
  po::options_description general_options("");
  general_options.add_options()
//     ("cnet,c", po::value(&opt.cnet_file),
//      "Load a control network from a file (optional).")
    ("output-prefix,o", po::value(&opt.out_prefix), "Prefix for output filenames.")
    ("bundle-adjuster", po::value(&opt.ba_type)->default_value("Ceres"),
     "Choose a solver from: Ceres, RobustSparse, RobustRef, Sparse, Ref.")
    ("cost-function", po::value(&opt.cost_function)->default_value("Cauchy"),
     "Choose a cost function from: Cauchy, PseudoHuber, Huber, L1, L2.")
    ("robust-threshold", po::value(&opt.robust_threshold)->default_value(0.5),
     "Set the threshold for robust cost functions.")
    ("datum", po::value(&opt.datum_str)->default_value(""), "Use this datum (needed only if ground control points are used). [WGS_1984, D_MOON (radius is assumed to be 1,737,400 meters), D_MARS (radius is assumed to be 3,396,190 meters), etc.]")
    ("semi-major-axis", po::value(&opt.semi_major)->default_value(0),
     "Explicitly set the datum semi-major axis in meters (needed only if ground control points are used).")
    ("semi-minor-axis", po::value(&opt.semi_minor)->default_value(0),
     "Explicitly set the datum semi-minor axis in meters (needed only if ground control points are used).")
    ("session-type,t", po::value(&opt.stereo_session_string)->default_value(""),
     "Select the stereo session type to use for processing. Options: pinhole isis dg rpc. Usually the program can select this automatically by the file extension.")
    ("min-matches", po::value(&opt.min_matches)->default_value(30),
     "Set the minimum  number of matches between images that will be considered.")
    ("max-iterations", po::value(&opt.max_iterations)->default_value(100),
     "Set the maximum number of iterations.")
    ("lambda,l", po::value(&opt.lambda)->default_value(-1),
     "Set the initial value of the LM parameter lambda (ignored for the Ceres solver).")
    ("report-level,r",po::value(&opt.report_level)->default_value(10),
     "Use a value >= 20 to get increasingly more verbose output.");
//     ("save-iteration-data,s", "Saves all camera information between iterations to output-prefix-iterCameraParam.txt, it also saves point locations for all iterations in output-prefix-iterPointsParam.txt.");
  general_options.add( asp::BaseOptionsDescription(opt) );

  po::options_description positional("");
  positional.add_options()
    ("input-files", po::value(&opt.image_files));

  po::positional_options_description positional_desc;
  positional_desc.add("input-files", -1);

  std::string usage("<images> <cameras> <optional ground control points> -o <output prefix> [options]");
  po::variables_map vm =
    asp::check_command_line(argc, argv, opt, general_options, general_options,
                            positional, positional_desc, usage);

  if ( opt.image_files.empty() )
    vw_throw( ArgumentErr() << "Missing input image files.\n"
              << usage << general_options );
  opt.gcp_files = sort_out_gcps( opt.image_files );
  opt.camera_files = sort_out_cameras( opt.image_files );

  
  if (!opt.gcp_files.empty()){
    // Need to read the datum if we have gcps.
    if (opt.datum_str != ""){
      // If the user set the datum, use it.
      opt.datum.set_well_known_datum(opt.datum_str);
    }else if (opt.semi_major > 0 && opt.semi_minor > 0){
      // Otherwise, if the user set the semi-axes, use that.
      opt.datum = cartography::Datum("User Specified Datum", "User Specified Spheroid",
                                     "Reference Meridian",
                                     opt.semi_major, opt.semi_minor, 0.0);
    }else{
      vw_throw( ArgumentErr() << "When ground control points are used, "
                << "the datum must be specified." );
    }
    vw_out() << "Will use datum: " << opt.datum << std::endl;
    
  }
  
  if ( opt.out_prefix.empty() )
    vw_throw( ArgumentErr() << "Missing output prefix." );

  // Create the output directory 
  asp::create_out_dir(opt.out_prefix);
    
  // Turn on logging to file
  asp::log_to_file(argc, argv, "", opt.out_prefix);

  opt.save_iteration = vm.count("save-iteration-data");
  boost::to_lower( opt.stereo_session_string );
  boost::to_lower( opt.ba_type );
  boost::to_lower( opt.cost_function );
  if ( !( opt.ba_type == "ceres"        ||
          opt.ba_type == "robustsparse" ||
          opt.ba_type == "robustref"    ||
          opt.ba_type == "sparse"       ||
          opt.ba_type == "ref" 
          ) )
    vw_throw( ArgumentErr() << "Unknown bundle adjustment version: " << opt.ba_type
              << ". Options are: [Ceres, RobustSparse, RobustRef, Sparse, Ref]\n" );
}

int main(int argc, char* argv[]) {

  Options opt;
  try {
    handle_arguments( argc, argv, opt );

    int num_images = opt.image_files.size();
    // Create the stereo session. Try to auto-guess the session type.
    if (num_images <= 1)
      vw_throw( ArgumentErr() << "Must have at least two image "
                << "files to do bundle adjustment.\n" );

    // If there are no camera files, then the image files have the
    // camera information.
    if (opt.camera_files.empty()){
      for (int i = 0; i < num_images; i++)
        opt.camera_files.push_back("");
    }

    // Sanity check
    if (num_images != (int)opt.camera_files.size())
      vw_throw(ArgumentErr() << "Must have as many cameras as we have images.\n");

    // Create the stereo session. This will attempt to identify the
    // session type.
    typedef boost::scoped_ptr<asp::StereoSession> SessionPtr;
    SessionPtr session
      (asp::StereoSession::create(opt.stereo_session_string, opt,
                                  opt.image_files[0], opt.image_files[1],
                                  opt.camera_files[0], opt.camera_files[1],
                                  opt.out_prefix
                                  ));
    
    // Read in the camera model and image info for the input images.
    for (int i = 0; i < num_images; i++){
      vw_out(DebugMessage,"asp") << "Loading: "
                                 << opt.image_files[i] << ' '
                                 << opt.camera_files[i] << "\n";
      opt.camera_models.push_back(session->camera_model(opt.image_files[i],
                                                        opt.camera_files[i]));
    }

    // Create the match points
    for (int i = 0; i < num_images; i++){
      for (int j = i+1; j < num_images; j++){
        std::string image1 = opt.image_files[i];
        std::string image2 = opt.image_files[j];
        std::string match_filename
          = ip::match_filename(opt.out_prefix, image1, image2);
        boost::shared_ptr<DiskImageResource>
          rsrc1( DiskImageResource::open(image1) ),
          rsrc2( DiskImageResource::open(image2) );
        float nodata1, nodata2;
        session->get_nodata_values(rsrc1, rsrc2, nodata1, nodata2);
        session->ip_matching( image1, image2, nodata1, nodata2, match_filename,
                              opt.camera_models[i].get(), opt.camera_models[j].get());
      }
    }
    
    opt.cnet.reset( new ControlNetwork("BundleAdjust") );
    if ( opt.cnet_file.empty() ) {
      build_control_network( (*opt.cnet), opt.camera_models,
                             opt.image_files,
                             opt.min_matches,
                             std::vector<std::string>(),
                             opt.out_prefix
                             );
      add_ground_control_points( (*opt.cnet), opt.image_files,
                                 opt.gcp_files.begin(), opt.gcp_files.end(),
                                 opt.datum
                                 );
      
      opt.cnet->write_binary(opt.out_prefix + "-control");
    } else  {
      vw_out() << "Loading control network from file: "
               << opt.cnet_file << "\n";

      // Deciding which Control Network we have
      std::vector<std::string> tokens;
      boost::split( tokens, opt.cnet_file, boost::is_any_of(".") );
      if ( tokens.back() == "net" ) {
        // An ISIS style control network
        opt.cnet->read_isis( opt.cnet_file );
      } else if ( tokens.back() == "cnet" ) {
        // A VW binary style
        opt.cnet->read_binary( opt.cnet_file );
      } else {
        vw_throw( IOErr() << "Unknown Control Network file extension, \""
                  << tokens.back() << "\"." );
      }
    }
    
    typedef BundleAdjustmentModel ModelType;
    if ( opt.cost_function == "cauchy" ) {
      do_ba_costfun<ModelType, CauchyError>( CauchyError(opt.robust_threshold), opt );
    }else if ( opt.cost_function == "pseudohuber" ) {
      do_ba_costfun<ModelType, PseudoHuberError>( PseudoHuberError(opt.robust_threshold), opt );
    } else if ( opt.cost_function == "huber" ) {
      do_ba_costfun<ModelType, HuberError>( HuberError(opt.robust_threshold), opt );
    } else if ( opt.cost_function == "l1" ) {
      do_ba_costfun<ModelType, L1Error>( L1Error(), opt );
    } else if ( opt.cost_function == "l2" ) {
      do_ba_costfun<ModelType, L2Error>( L2Error(), opt );
    }else{
      vw_throw( ArgumentErr() << "Unknown cost function: " << opt.cost_function
                << ". Options are: Cauchy, PseudoHuber, Huber, L1, L2.\n" );
    }
      
  } ASP_STANDARD_CATCHES;
}
