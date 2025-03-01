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


/// \file StereoSessionNadirPinhole.h
///
/// This is a specialization for downward facing pinhole camera model
/// on Earth. In other word images that we are sure not going to be
/// looking at the horizon. If that is the case, we can be smarter
/// about interest point gathering.
///
/// This session is specifically meant for testing with KH9 imagery

#ifndef __STEREO_SESSION_NADIR_PINHOLE_H__
#define __STEREO_SESSION_NADIR_PINHOLE_H__

#include <asp/Sessions/Pinhole/StereoSessionPinhole.h>

namespace asp {

  class StereoSessionNadirPinhole : public StereoSessionPinhole {
  public:
    virtual ~StereoSessionNadirPinhole() {}

    virtual std::string name() const { return "nadirpinhole"; }

    // For reversing our arithmetic applied in preprocessing.
    typedef vw::HomographyTransform left_tx_type;
    typedef vw::HomographyTransform right_tx_type;
    typedef vw::stereo::StereoModel stereo_model_type;
    left_tx_type tx_left() const;
    right_tx_type tx_right() const;

    // Specialization for how interest points are found
    virtual bool ip_matching(std::string const& input_file1,
                             std::string const& input_file2,
                             float nodata1, float nodata2,
                             std::string const& match_filename,
                             vw::camera::CameraModel* cam1,
                             vw::camera::CameraModel* cam2);

    // Stage 1: Preprocessing
    virtual void pre_preprocessing_hook( std::string const& left_input_file,
                                         std::string const& right_input_file,
                                         std::string &left_output_file,
                                         std::string &right_output_file);

    static StereoSession* construct() { return new StereoSessionNadirPinhole; }
  };
}

#endif//__STEREO_SESSION_NADIR_PINHOLE_H__
