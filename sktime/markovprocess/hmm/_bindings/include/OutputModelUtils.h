//
// Created by mho on 2/3/20.
//

#pragma once

#include <random>
#include <thread>
#include <iomanip>

#include "common.h"

namespace hmm {
namespace output_models {

template<typename dtype>
void handleOutliers(np_array<dtype> &outputProbabilityTrajectory) {
    auto nTimesteps = outputProbabilityTrajectory.shape(0);
    auto nStates = outputProbabilityTrajectory.shape(1);
    auto ptr = outputProbabilityTrajectory.mutable_data();

    #pragma omp parallel for
    for (decltype(nTimesteps) t = 0; t < nTimesteps; ++t) {
        auto begin = ptr + t * nStates;
        auto end = begin + nStates;
        auto sum = std::accumulate(begin, end, static_cast<dtype>(0.));
        if (sum == 0) {
            // got an outlier, fill with uniform (will be renormalized later)
            std::fill(begin, end, static_cast<dtype>(1));
        }
    }

}


namespace discrete {

template<typename dtype, typename State>
np_array<dtype> generateObservationTrajectory(const np_array<State> &hiddenStateTrajectory,
                                              const np_array<dtype> &outputProbabilities) {
    if (hiddenStateTrajectory.ndim() != 1) {
        throw std::invalid_argument("generate observation trajectory needs 1-dimensional hidden state trajectory");
    }
    auto nTimesteps = hiddenStateTrajectory.shape(0);
    auto nObs = outputProbabilities.shape(1);

    np_array<dtype> output({static_cast<std::size_t>(nTimesteps)});
    auto outputPtr = output.mutable_data();

    #pragma omp parallel
    {
        std::default_random_engine generator(clock() + std::hash<std::thread::id>()(std::this_thread::get_id()));
        std::discrete_distribution<> ddist;

        #pragma omp for
        for (decltype(nTimesteps) t = 0; t < nTimesteps; ++t) {
            auto state = hiddenStateTrajectory.at(t);
            auto begin = outputProbabilities.data(state, 0);
            auto end = begin + nObs;
            ddist.param(decltype(ddist)::param_type(begin, end));
            auto obs = ddist(generator);
            *(outputPtr + t) = obs;
        }
    }

    return output;
}

template<typename dtype, typename State>
np_array<dtype> toOutputProbabilityTrajectory(const np_array<State> &observations,
                                              const np_array<dtype> &outputProbabilities) {
    if (observations.ndim() != 1) {
        throw std::invalid_argument("observations trajectory needs to be one-dimensional.");
    }
    auto nHidden = static_cast<std::size_t>(outputProbabilities.shape(0));

    np_array<dtype> output(std::vector<std::size_t>{static_cast<std::size_t>(observations.shape(0)), nHidden});
    auto outputPtr = output.mutable_data(0);

    #pragma omp parallel for
    for (ssize_t t = 0; t < observations.shape(0); ++t) {
        auto obsState = observations.at(t);
        for (std::size_t i = 0; i < nHidden; ++i) {
            *(outputPtr + t * nHidden + i) = outputProbabilities.at(i, obsState);
        }
    }

    return output;
}

template<typename dtype, typename State>
void sample(const std::vector<np_array<State>> &observationsPerState, np_array<dtype> &outputProbabilities,
            const np_array<dtype> &prior) {
    auto nObs = outputProbabilities.shape(1);
    ssize_t currentState{0};

    std::default_random_engine generator(clock() + std::hash<std::thread::id>()(std::this_thread::get_id()));
    dirichlet_distribution<dtype> dirichlet;

    for (const np_array<State> &observations : observationsPerState) {
        std::vector<dtype> hist(nObs, 0);
        for (ssize_t i = 0; i < observations.size(); ++i) {
            ++hist.at(observations.at(i));
        }
        auto priorBegin = prior.data(currentState);
        // add prior onto histogram
        std::transform(hist.begin(), hist.end(), priorBegin, hist.begin(), std::plus<>());
        dirichlet.params(hist.begin(), hist.end());
        auto probs = dirichlet(generator);

        for (std::size_t i = 0; i < probs.size(); ++i) {
            //if(probs[i] != 0) {
            outputProbabilities.mutable_at(currentState, i) = probs[i];
            //}
        }

        ++currentState;
    }
}

template<typename dtype, typename State>
void updatePOut(const np_array<State> &obs, const np_array<dtype> &weights, np_array<dtype> &pout) {
    auto T = static_cast<std::size_t>(obs.size());
    auto N = static_cast<std::size_t>(pout.shape(0));
    auto M = static_cast<std::size_t>(pout.shape(1));

    auto obsBuf = obs.data();
    auto weightsBuf = weights.data();
    auto poutBuf = pout.mutable_data();

    for (std::size_t t = 0; t < T; ++t) {
        auto o = obsBuf[t];
        for (std::size_t i = 0; i < N; ++i) {
            poutBuf[i * M + o] += weightsBuf[t * N + i];
        }
    }
}

}

namespace gaussian {


template<typename dtype>
constexpr dtype pi() { return 3.141592653589793238462643383279502884e+00; }

/**
 * Returns the probability density of a Gaussian with given mu and sigma evaluated at o
 * @tparam dtype data type
 * @param o observation value
 * @param mu mean value
 * @param sigma standard deviation
 */
template<typename dtype>
constexpr dtype sample(dtype o, dtype mu, dtype sigma) {
    double c = 1.0 / (std::sqrt(2.0 * pi<dtype>()) * sigma);
    double d = (o - mu) / sigma;
    return c * exp(-0.5 * d * d);
}

template<typename dtype>
np_array<dtype> pO(dtype o, const np_array<dtype> &mus, const np_array<dtype> &sigmas, py::object out) {
    auto N = static_cast<std::size_t>(mus.shape(0));

    np_array<dtype> p;
    if (!out.is_none()) {
        p = py::cast<np_array<dtype>>(out);
    } else {
        p = np_array<dtype>({N});
    }
    auto pBuf = p.mutable_data();
    auto musBuf = mus.data();
    auto sigmasBuf = sigmas.data();

    #pragma omp parallel for
    for (std::size_t i = 0; i < N; ++i) {
        pBuf[i] = sample(o, musBuf[i], sigmasBuf[i]);
    }

    return p;
}

template<typename dtype>
np_array<dtype> toOutputProbabilityTrajectory(const np_array<dtype> &obs, const np_array<dtype> &mus,
                                              const np_array<dtype> &sigmas) {
    auto N = static_cast<std::size_t>(mus.shape(0));
    auto T = static_cast<std::size_t>(obs.shape(0));

    np_array<dtype> p({T, N});
    auto obsBuf = obs.data();
    auto musBuf = mus.data();
    auto sigmasBuf = sigmas.data();
    auto pBuf = p.mutable_data();

    #pragma omp parallel for collapse(2)
    for (std::size_t t = 0; t < T; ++t) {
        for (std::size_t i = 0; i < N; ++i) {
            pBuf[t * N + i] = sample(obsBuf[t], musBuf[i], sigmasBuf[i]);
        }
    }

    return p;
}

template<typename dtype>
np_array<dtype>
generateObservationTrajectory(const np_array<dtype> &hiddenStateTrajectory, const np_array<dtype> &means,
                              const np_array<dtype> &sigmas) {
    if (hiddenStateTrajectory.ndim() != 1) {
        throw std::invalid_argument("Hidden state trajectory must be one-dimensional!");
    }
    auto nTimesteps = hiddenStateTrajectory.shape(0);
    np_array<dtype> output({static_cast<std::size_t>(nTimesteps)});
    auto ptr = output.mutable_data();

    std::default_random_engine generator(clock() + std::hash<std::thread::id>()(std::this_thread::get_id()));
    std::normal_distribution<dtype> dist{0, 1};

    for (decltype(nTimesteps) t = 0; t < nTimesteps; ++t) {
        auto state = hiddenStateTrajectory.at(t);
        *(ptr + t) = sigmas.at(state) * dist(generator) + means.at(state);
    }
    return output;
}

template<typename dtype>
std::tuple<np_array<dtype>, np_array<dtype>> fit(std::size_t nHiddenStates, const py::list &observations,
                                                 const py::list &weights) {
    auto nObsTrajs = observations.size();
    if (nObsTrajs != weights.size()) {
        throw std::invalid_argument("number of observation trajectories must match number of weight matrices");
    }

    auto result = std::make_tuple(
            np_array<dtype>(std::vector<std::size_t>{nHiddenStates}),
            np_array<dtype>(std::vector<std::size_t>{nHiddenStates})
    );
    auto &means = std::get<0>(result);
    auto &sigmas = std::get<1>(result);

    std::fill(means.mutable_data(), means.mutable_data() + nHiddenStates, 0);
    std::fill(sigmas.mutable_data(), sigmas.mutable_data() + nHiddenStates, 0);

    // fit means
    {
        std::vector<dtype> wSum(nHiddenStates, 0);
        auto weightsIt = weights.begin();
        auto obsIt = observations.begin();
        for (decltype(nObsTrajs) k = 0; k < nObsTrajs; ++k, ++weightsIt, ++obsIt) {
            const auto &w = py::cast<np_array<dtype>>(*weightsIt);
            const auto &obs = py::cast<np_array<dtype>>(*obsIt);
            for (decltype(nHiddenStates) i = 0; i < nHiddenStates; ++i) {
                dtype dot = 0;
                dtype wStateSum = 0;
                for (ssize_t t = 0; t < obs.shape(0); ++t) {
                    dot += w.at(t, i) * obs.at(t);
                    wStateSum += w.at(t, i);
                }
                // update nominator
                means.mutable_at(i) += dot;
                // update denominator
                wSum.at(i) += wStateSum;
            }

        }
        // update normalize
        for(decltype(nHiddenStates) i = 0; i < nHiddenStates; ++i) {
            means.mutable_at(i) /= wSum.at(i);
        }
    }
    // fit variances
    {
        std::vector<dtype> wSum(nHiddenStates, 0);
        auto weightsIt = weights.begin();
        auto obsIt = observations.begin();
        for (decltype(nObsTrajs) k = 0; k < nObsTrajs; ++k, ++weightsIt, ++obsIt) {
            const auto &w = py::cast<np_array<dtype>>(*weightsIt);
            const auto &obs = py::cast<np_array<dtype>>(*obsIt);

            for (decltype(nHiddenStates) i = 0; i < nHiddenStates; ++i) {
                dtype wStateSum = 0;
                dtype sigmaUpdate = 0;
                for (ssize_t t = 0; t < obs.shape(0); ++t) {
                    auto sqrty = static_cast<dtype>(obs.at(t)) - static_cast<dtype>(means.at(i));
                    sigmaUpdate += w.at(t, i) * sqrty*sqrty;
                    wStateSum += w.at(t, i);
                }
                // update nominator
                sigmas.mutable_at(i) += sigmaUpdate;
                // update denominator
                wSum.at(i) += wStateSum;
            }
        }
        for(decltype(nHiddenStates) i = 0; i < nHiddenStates; ++i) {
            sigmas.mutable_at(i) = std::sqrt(sigmas.at(i) / wSum.at(i));
        }
    }
    return result;
}

}

}
}
