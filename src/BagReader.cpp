/*
 * Copyright (c) 2011, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <ecto/cell.hpp>
#include <ecto/ecto.hpp>
#include <ros/ros.h>

#include <rosbag/bag.h>
#include <rosbag/query.h>
#include <rosbag/view.h>

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <string>

#include <boost/foreach.hpp>
#include <boost/python/stl_iterator.hpp>
#include <ecto_ros/wrap_bag.hpp>

#include <boost/random.hpp>

namespace ecto_ros {
namespace bp = boost::python;

using ecto::tendrils;
struct BagReader {

  static void declare_params(tendrils &params) {
    params.declare<bp::object>("baggers", "A python dict Bagger_MessageT objects.").required(true);
    params.declare<std::string>("bag", "The bag filename.", "foo.bag").required(true);
    params.declare(&BagReader::random_access_, "random_access", "Access bag at random times.", false);
  }

  static void declare_io(const tendrils &p, tendrils &in, tendrils &out) {
    bp::object baggers;
    p["baggers"] >> baggers;
    if (!baggers || baggers == bp::object())
      return;
    bp::list l = bp::dict(baggers).items();
    for (int j = 0, end = bp::len(l); j < end; ++j) {
      bp::object key = l[j][0];
      bp::object value = l[j][1];
      std::string keystring = bp::extract<std::string>(key);
      bp::object cell_impl = getattr(value, "__impl");
      ecto::cell::ptr cell = bp::extract<ecto::cell::ptr>(cell_impl);
      Bagger_base::ptr bagger;
      cell->parameters["bagger"] >> bagger;
      out.declare(keystring, bagger->instantiate());
    }
  }

  void configure(const tendrils &p, const tendrils &in, const tendrils &out) {
    ECTO_SCOPED_CALLPYTHON();
    bp::object subs = p.get<bp::object>("baggers");
    bp::list l = bp::dict(subs).items();
    for (int j = 0; j < bp::len(l); ++j) {
      bp::object key = l[j][0];
      bp::object value = l[j][1];
      std::string keystring = bp::extract<std::string>(key);
      bp::object cell_impl = getattr(value, "__impl");
      ecto::cell::ptr cell = bp::extract<ecto::cell::ptr>(cell_impl);
      std::string topic;
      cell->parameters["topic_name"] >> topic;
      Bagger_base::ptr bagger;
      cell->parameters["bagger"] >> bagger;
      topics_.push_back(topic);
      baggers_[topic] = std::make_pair(keystring, bagger);
    }
    p["bag"]->set_callback<std::string>(
        boost::bind(&BagReader::on_bag_name_change, this, _1));
  }

  void on_bag_name_change(const std::string &bag) {
    if (bag_name_ != bag) {
      std::cout << "Opening bag: " << bag << std::endl;
      bag_name_ = bag;
      bag_.open(bag_name_, rosbag::bagmode::Read);
      view_.addQuery(bag_, rosbag::TopicQuery(topics_));
      message_ = view_.begin();
      if (message_ == view_.end()) {
        throw std::runtime_error("Your bag is empty!");
      }
    }
  }

  double get_random_time()
  {
      const double rangeMin = view_.getBeginTime().toSec();
      const double rangeMax = view_.getEndTime().toSec();

      typedef boost::uniform_real<> NumberDistribution;
      typedef boost::mt19937 RandomNumberGenerator;
      typedef boost::variate_generator<RandomNumberGenerator&, NumberDistribution> Generator;

      NumberDistribution distribution(rangeMin, rangeMax);
      RandomNumberGenerator generator;
      Generator numberGenerator(generator, distribution);
      generator.seed(std::time(0));

      return numberGenerator();
  }

  int output_random(const tendrils &out) {
    double random_time = get_random_time();
    std::set<std::string> counter;

    rosbag::View::iterator message = view_.begin();

    while (message != view_.end()) {
      if (message->getTime().toSec() > random_time) {
        if (counter.size() != topics_.size()) {
          std::string topic = message->getTopic();
          if (!counter.insert(topic).second) {
            std::cout << "Warning: More than one message from topic: " << topic
                      << " <<<< Overwriting last seen message." << std::endl;
          }

          Bagger_base::ptr bagger;
          std::string key;
          boost::tie(key, bagger) = baggers_[topic];
          out[key] << bagger->instantiate(message);
        } else {
          std::cout << "Topics found, returning" << '\n';
          return ecto::OK;
        }
      }
      ++message;
    }

    std::cout << "End of bag." << std::endl;
    return ecto::QUIT;
  }

  int output_sequential(const tendrils &out) {
    if (message_ == view_.end()) {
      std::cout << "End of bag." << std::endl;
      return ecto::QUIT;
    }
    std::set<std::string> counter;
    while (counter.size() != topics_.size() && message_ != view_.end()) {
      std::string topic = (*message_).getTopic();
      if (!counter.insert(topic).second) {
        std::cout << "Warning: More than one message from topic: " << topic
                  << " <<<< Overwriting last seen message." << std::endl;
      }
      Bagger_base::ptr bagger;
      std::string key;
      boost::tie(key, bagger) = baggers_[topic];
      out[key] << bagger->instantiate(message_);
      ++message_;
    }

    return ecto::OK;
  }

  int process(const tendrils &in, const tendrils &out) {
    std::cout << "Random access: " << *random_access_ << '\n';
    if (*random_access_) {
      return output_random(out);
    } else {
      return output_sequential(out);
    }
  }

  std::vector<std::string> topics_;
  std::map<std::string, std::pair<std::string, Bagger_base::ptr> > baggers_;
  std::string bag_name_;
  rosbag::Bag bag_;
  rosbag::View view_;
  rosbag::View::iterator message_;
  ecto::spore<bool> random_access_;
};

} // namespace ecto_ros

ECTO_CELL(ecto_ros_main, ecto_ros::BagReader, "BagReader",
          "BagReader reads bags.")
