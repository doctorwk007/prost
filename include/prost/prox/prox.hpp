/**
* This file is part of prost.
*
* Copyright 2016 Thomas Möllenhoff <thomas dot moellenhoff at in dot tum dot de> 
* and Emanuel Laude <emanuel dot laude at in dot tum dot de> (Technical University of Munich)
*
* prost is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* prost is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with prost. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PROST_PROX_HPP_
#define PROST_PROX_HPP_

#include <thrust/device_vector.h>
#include "prost/common.hpp"

namespace prost {

using thrust::device_vector;

template<typename T> class ProxMoreau;
template<typename T> class ProxPermute;
template<typename T> class ProxTransform;

///
/// \brief Virtual base class for all proximal operators. 
///
template<typename T>
class Prox {
  friend class ProxMoreau<T>;
  friend class ProxPermute<T>;
  friend class ProxTransform<T>;
  
public:
  Prox(size_t index, size_t size, bool diagsteps) :
    index_(index),
    size_(size),
    diagsteps_(diagsteps) { }

  Prox(const Prox<T>& other) :
    index_(other.index_),
    size_(other.size_),
    diagsteps_(other.diagsteps_) { }
  
  virtual ~Prox() { }

  /// \brief Initializes the prox Operator.
  virtual void Initialize() { }

  /// \brief Cleans up any data.
  virtual void Release() { }

  /// 
  /// \brief Evaluates the prox operator on the GPU.
  /// 
  /// \param Result of prox.
  /// \param Proximal operator argument.
  /// \param Diagonal step sizes.
  /// \param Scalar step size.
  ///
  void Eval(
    thrust::device_vector<T>& result, 
    const thrust::device_vector<T>& arg, 
    const thrust::device_vector<T>& tau_diag, 
    T tau,
    bool invert_tau = false);

  /// 
  /// \brief Evaluates the prox operator on the GPU, using CPU data. Mainly 
  ///        for debugging purposes.
  /// 
  /// \param Result of prox.
  /// \param Proximal operator argument.
  /// \param Diagonal step sizes.
  /// \param Scalar step size.
  /// \returns GPU time of Prox in milliseconds.
  ///
  double Eval(
    std::vector<T>& result, 
    const std::vector<T>& arg, 
    const std::vector<T>& tau_diag, 
    T tau); 

  virtual size_t gpu_mem_amount() const = 0;
  size_t index() const { return index_; }
  size_t size() const { return size_; }
  size_t end() const { return index_ + size_ - 1; }
  bool diagsteps() const { return diagsteps_; }
  
  /// \brief Returns the separability information of the prox operator. 
  ///        Needed for averaging the preconditioners
  virtual void get_separable_structure(
    vector<std::tuple<size_t, size_t, size_t> >& sep);
  
protected:
  /// 
  /// \brief Evaluates the prox operator on the GPU, local meaning that
  ///        arg, res and tau point to the place in memory where the
  ///        prox begins.
  /// 
  /// \param Result of prox.
  /// \param Proximal operator argument.
  /// \param Diagonal step sizes.
  /// \param Scalar step size.
  /// \param Perform the prox with inverted step sizes?
  /// 
  virtual void EvalLocal(
    const typename thrust::device_vector<T>::iterator& result_beg,
    const typename thrust::device_vector<T>::iterator& result_end,
    const typename thrust::device_vector<T>::const_iterator& arg_beg,
    const typename thrust::device_vector<T>::const_iterator& arg_end,
    const typename thrust::device_vector<T>::const_iterator& tau_beg,
    const typename thrust::device_vector<T>::const_iterator& tau_end,
    T tau,
    bool invert_tau) = 0;
  
  /// \brief Index where prox-Operator starts.
  size_t index_; 

  /// \brief Dimension of the function domain. 
  size_t size_;

  /// \brief Able to handle diagonal matrices as step size?
  bool diagsteps_; 
};

} // namespace prost

#endif // PROST_PROX_HPP_
