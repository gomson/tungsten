#include "ReversibleJumpMltIntegrator.hpp"

#include "sampling/SobolPathSampler.hpp"

#include "cameras/Camera.hpp"

#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"

namespace Tungsten {

ReversibleJumpMltIntegrator::ReversibleJumpMltIntegrator()
: Integrator(),
  _w(0),
  _h(0),
  _sampler(0xBA5EBA11)
{
}

void ReversibleJumpMltIntegrator::saveState(OutputStreamHandle &/*out*/)
{
    FAIL("ReversibleJumpMltIntegrator::saveState not supported!");
}

void ReversibleJumpMltIntegrator::loadState(InputStreamHandle &/*in*/)
{
    FAIL("ReversibleJumpMltIntegrator::loadState not supported!");
}

void ReversibleJumpMltIntegrator::traceSamplePool(uint32 taskId, uint32 numSubTasks, uint32 /*threadId*/)
{
    uint32 rayBase = intLerp(0, _settings.initialSamplePool, taskId + 0, numSubTasks);
    uint32 rayTail = intLerp(0, _settings.initialSamplePool, taskId + 1, numSubTasks);

    LightPath  cameraPath(_settings.maxBounces + 1);
    LightPath emitterPath(_settings.maxBounces);

    uint32 candidateIdx = rayBase;
    uint32 rayIdx = 0;
    while (rayIdx < rayTail - rayBase && candidateIdx < rayTail) {
        uint64  cameraState = _tracers[taskId]-> cameraSampler().sampler().state();
        uint64 emitterState = _tracers[taskId]->emitterSampler().sampler().state();
        uint32 sequence     = _tracers[taskId]-> cameraSampler().sampler().sequence();

        _tracers[taskId]->traceCandidatePath(cameraPath, emitterPath, [&](Vec3f value, int s, int t) {
            if (candidateIdx == rayTail)
                return;
            int idx = candidateIdx++;

            _pathCandidates[idx].cameraState = cameraState;
            _pathCandidates[idx].emitterState = emitterState;
            _pathCandidates[idx].sequence = sequence;
            _pathCandidates[idx].luminance = value.luminance();
            if (std::isnan(_pathCandidates[idx].luminance))
                _pathCandidates[idx].luminance = 0.0f;
            _pathCandidates[idx].luminanceSum = _pathCandidates[idx].luminance;
            _pathCandidates[idx].s = s;
            _pathCandidates[idx].t = t;
        });

        rayIdx++;
    }

    _subtaskData[taskId].rangeStart = rayBase;
    _subtaskData[taskId].rangeLength = candidateIdx - rayBase;
    _subtaskData[taskId].raysCast = rayIdx;
}

void ReversibleJumpMltIntegrator::runSampleChain(uint32 taskId, uint32 numSubTasks, uint32 /*threadId*/)
{
    uint32 rayCount = _w*_h*(_nextSpp - _currentSpp);

    uint32 rayBase    = intLerp(0, rayCount, taskId + 0, numSubTasks);
    uint32 raysToCast = intLerp(0, rayCount, taskId + 1, numSubTasks) - rayBase;

    MultiplexedStats stats(*_stats);
    for (int i = 1; i <= _settings.maxBounces; ++i) {
        int chainLength = int(raysToCast*_luminancePerLength[i]/_luminanceScale);
        if (chainLength > 0)
            _tracers[taskId]->runSampleChain(i, chainLength, stats, _luminanceScale);
    }
}

void ReversibleJumpMltIntegrator::selectSeedPaths()
{
    uint32 rangeTail = _subtaskData[0].rangeLength;
    uint32 numRays = _subtaskData[0].raysCast;
    for (size_t i = 1; i < _subtaskData.size(); ++i) {
        SubtaskData data = _subtaskData[i];
        if (rangeTail != data.rangeStart)
            std::memmove(&_pathCandidates[rangeTail], &_pathCandidates[data.rangeStart], data.rangeLength*sizeof(PathCandidate));
        rangeTail += data.rangeLength;
        numRays += data.raysCast;
    }

    _luminancePerLength.clear();
    _luminancePerLength.resize(_settings.maxBounces + 1, 0.0);
    for (uint32 i = 1; i < rangeTail; ++i) {
        int length = _pathCandidates[i].s + _pathCandidates[i].t - 1;
        _pathCandidates[i].luminanceSum = _pathCandidates[i].luminance + _luminancePerLength[length];
        _luminancePerLength[length] = _pathCandidates[i].luminanceSum;
    }

    for (size_t tracerId = 0; tracerId < _tracers.size(); ++tracerId) {
        std::vector<double> targetEnergy;
        for (size_t i = 0; i < _luminancePerLength.size(); ++i)
            targetEnergy.push_back(_sampler.next1D()*_luminancePerLength[i]);

        std::vector<int> selectedPaths(_luminancePerLength.size(), -1);
        for (uint32 i = 0; i < rangeTail; ++i) {
            int length = _pathCandidates[i].s + _pathCandidates[i].t - 1;
            if (selectedPaths[length] == -1 && targetEnergy[length] < _pathCandidates[i].luminanceSum) {
                selectedPaths[length] = i;

                const PathCandidate &candidate = _pathCandidates[i];
                UniformSampler  cameraReplaySampler(candidate. cameraState, candidate.sequence);
                UniformSampler emitterReplaySampler(candidate.emitterState, candidate.sequence + 1);
                _tracers[tracerId]->startSampleChain(candidate.s, candidate.t, candidate.luminance,
                        cameraReplaySampler, emitterReplaySampler);
            }
        }
    }

    double totalLuminance = 0.0;
    for (double &l : _luminancePerLength) {
        l /= numRays;
        totalLuminance += l;
    }

    _scene->cam().blitSplatBuffer();
    _luminanceScale = totalLuminance;
}

void ReversibleJumpMltIntegrator::fromJson(JsonPtr v, const Scene &/*scene*/)
{
    _settings.fromJson(v);
}

rapidjson::Value ReversibleJumpMltIntegrator::toJson(Allocator &allocator) const
{
    return _settings.toJson(allocator);
}

void ReversibleJumpMltIntegrator::saveOutputs()
{
    Integrator::saveOutputs();
    if (_imagePyramid)
        _imagePyramid->saveBuffers(_scene->rendererSettings().outputFile().stripExtension(), _scene->rendererSettings().spp(), true);
}

void ReversibleJumpMltIntegrator::prepareForRender(TraceableScene &scene, uint32 seed)
{
    _chainsLaunched = false;
    _currentSpp = 0;
    _sampler = UniformSampler(MathUtil::hash32(seed), ThreadUtils::pool->threadCount()*3);
    _scene = &scene;
    advanceSpp();

    _w = scene.cam().resolution().x();
    _h = scene.cam().resolution().y();
    scene.cam().requestSplatBuffer();

    _stats.reset(new AtomicMultiplexedStats(_settings.maxBounces));

    if (_settings.imagePyramid)
        _imagePyramid.reset(new ImagePyramid(_settings.maxBounces, _scene->cam()));

    for (uint32 i = 0; i < ThreadUtils::pool->threadCount(); ++i) {
        _tracers.emplace_back(new ReversibleJumpMltTracer(&scene, _settings, i, _sampler, _imagePyramid.get()));
        _subtaskData.emplace_back();
    }
}

void ReversibleJumpMltIntegrator::teardownAfterRender()
{
    for (int length = 0; length <= _settings.maxBounces; ++length) {
        int mutLarge = _stats->largeStep().numMutations(length);
        int mutSmall = _stats->smallStep().numMutations(length);
        int mutStrat = _stats->techniqueChange().numMutations(length);
        int inversion = _stats->inversion().numMutations(length);

        if (mutLarge + mutSmall + mutStrat) {
            std::cout << tfm::format(
                "Path length %2d:\n"
                "          Large step: acceptance ratio %5.2f%% of %d attempts\n"
                "          Small step: acceptance ratio %5.2f%% of %d attempts\n"
                "    Technique change: acceptance ratio %5.2f%% of %d attempts\n"
                "          Inversions: acceptance ratio %5.2f%% of %d attempts\n",
                length,
                mutLarge  == 0 ? 0.0f : 100.0f*_stats->largeStep().acceptanceRatio(length), mutLarge,
                mutSmall  == 0 ? 0.0f : 100.0f*_stats->smallStep().acceptanceRatio(length), mutSmall,
                mutStrat  == 0 ? 0.0f : 100.0f*_stats->techniqueChange().acceptanceRatio(length), mutStrat,
                inversion == 0 ? 0.0f : 100.0f*_stats->inversion().acceptanceRatio(length), inversion) << std::endl;
        }
    }

    _group.reset();

    _subtaskData.clear();
    _tracers.clear();

    _luminancePerLength.clear();
    _pathCandidates.reset();

    _stats.reset();
    _imagePyramid.reset();
}

void ReversibleJumpMltIntegrator::advanceSpp()
{
    int sppStep = _scene->rendererSettings().sppStep();
    int ipb = _settings.iterationsPerBatch;
    int finishBorder = _scene->rendererSettings().spp();
    int retraceBorder = ipb <= 0 ? finishBorder : (((int(_currentSpp) + ipb - 1)/ipb)*ipb);

    if (ipb > 0 && int(_currentSpp) == retraceBorder) {
        if (_chainsLaunched)
            _chainsLaunched = false;
        retraceBorder += ipb;
    }
    _nextSpp = min(int(_currentSpp) + sppStep, finishBorder, retraceBorder);
}

void ReversibleJumpMltIntegrator::startRender(std::function<void()> completionCallback)
{
    if (_chainsLaunched && done()) {
        completionCallback();
        return;
    }

    double weight = double(_w*_h)/(_w*_h*_nextSpp + _settings.initialSamplePool);
    _scene->cam().setSplatWeight(weight);

    using namespace std::placeholders;
    if (!_chainsLaunched) {
        if (!_pathCandidates)
            _pathCandidates.reset(new PathCandidate[_settings.initialSamplePool]);

        _group = ThreadUtils::pool->enqueue(
            std::bind(&ReversibleJumpMltIntegrator::traceSamplePool, this, _1, _2, _3),
            _tracers.size(),
            [&, completionCallback]() {
                selectSeedPaths();
                advanceSpp();
                _chainsLaunched = true;
                completionCallback();
            }
        );
    } else {
        _group = ThreadUtils::pool->enqueue(
            std::bind(&ReversibleJumpMltIntegrator::runSampleChain, this, _1, _2, _3),
            _tracers.size(),
            [&, completionCallback]() {
                _currentSpp = _nextSpp;
                advanceSpp();
                completionCallback();
            }
        );
    }
}

void ReversibleJumpMltIntegrator::waitForCompletion()
{
    if (_group) {
        _group->wait();
        _group.reset();
    }
}

void ReversibleJumpMltIntegrator::abortRender()
{
    if (_group) {
        _group->abort();
        _group->wait();
        _group.reset();
    }
}

}