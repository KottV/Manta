#pragma once

#include "../arch/Interpolation.h"

#include "Filter.h"
#include "PRM.h"
#include "Phasor.h"
#include "XenManager.h"
#include <functional>

namespace audio
{
	struct Manta
	{
		// enabled, cutoff, resonance, slope, feedback, oct, semi, drive, rm-depth, gain
		static constexpr int NumParametersPerLane = 10;

		static constexpr int NumLanes = 3;
		static constexpr int MaxSlopeStage = 4; //4*12db/oct

	private:
		class Filter
		{
			using Fltr = FilterBandpassSlope<MaxSlopeStage>;
		public:
			Filter() :
				filta()
			{}

			void operator()(float** laneBuf, float** samples, int numChannels, int numSamples,
				float* fcBuf, float* resoBuf, int stage) noexcept
			{
				{
					auto lane = laneBuf[0];
					auto smpls = samples[0];
					auto& fltr = filta[0];
					
					fltr.setStage(stage);

					for (auto s = 0; s < numSamples; ++s)
					{
						fltr.setFc(fcBuf[s], resoBuf[s]);
						lane[s] = fltr(smpls[s]);
					}
				}
				
				for (auto ch = 1; ch < numChannels; ++ch)
				{
					auto lane = laneBuf[ch];
					auto smpls = samples[ch];
					auto& fltr = filta[ch];

					for (auto s = 0; s < numSamples; ++s)
					{
						fltr.copy(filta[0]);
						lane[s] = fltr(smpls[s]);
					}
				}
			}

		protected:
			std::array<Fltr, 2> filta;
		};
		
		struct DelayFeedback
		{
			DelayFeedback() :
				ringBuffer(),
				size(0)
			{}

			void prepare(int delaySize)
			{
				size = delaySize;
				ringBuffer.setSize(2, size, false, true, false);
			}

			void operator()(float** samples, int numChannels, int numSamples, const int* wHead, const float* rHead,
				const float* feedback) noexcept
			{
				auto ringBuffr = ringBuffer.getArrayOfWritePointers();

				for (auto ch = 0; ch < numChannels; ++ch)
				{
					auto smpls = samples[ch];
					auto ring = ringBuffr[ch];

					for (auto s = 0; s < numSamples; ++s)
					{
						const auto w = wHead[s];
						const auto r = rHead[s];
						const auto fb = feedback[s];

						const auto sOut = interpolate::lerp(ring, r, size) * fb + smpls[s];
						const auto sIn = sOut;

						ring[w] = sIn;
						smpls[s] = sOut;
					}
				}
			}

			AudioBuffer ringBuffer;
			int size;
		};
		
		struct RingMod
		{
			RingMod() :
				phasor(),
				oscBuffer()
			{}

			void prepare(float Fs, int blockSize)
			{
				phasor.prepare(1.f / Fs);
				oscBuffer.resize(blockSize);
			}

			void operator()(float** samples, int numChannels, int numSamples,
				float* _rmDepth, float* _freqHz) noexcept
			{
				for (auto s = 0; s < numSamples; ++s)
				{
					const auto freqHz = _freqHz[s];
					phasor.setFrequencyHz(freqHz);
					phasor();
					oscBuffer[s] = std::cos(phasor.phase.phase * Tau);
				}

				for (auto ch = 0; ch < numChannels; ++ch)
				{
					auto smpls = samples[ch];

					for (auto s = 0; s < numSamples; ++s)
					{
						const auto rmd = _rmDepth[s];
						const auto dry = smpls[s];
						const auto wet = dry * oscBuffer[s];

						smpls[s] += rmd * (wet - dry);
					}
				}
						
			}

		protected:
			Phasor<float> phasor;
			std::vector<float> oscBuffer;
		};

		struct Lane
		{
			Lane() :
				laneBuffer(),
				readHead(),
				filter(),
				
				frequency(.5f),
				resonance(40.f),
				drive(0.f),
				feedback(0.f),
				delayRate(0.f),
				rmDepth(0.f),
				rmFreqHz(1.f),
				gain(1.f),
				
				delayFB(),
				ringMod(),

				Fs(1.f),
				delaySizeF(1.f)
			{}

			void prepare(float sampleRate, int blockSize, int delaySize)
			{
				Fs = sampleRate;

				ringMod.prepare(Fs, blockSize);

				laneBuffer.setSize(2, blockSize, false, true, false);

				frequency.prepare(Fs, blockSize, 10.f);
				resonance.prepare(Fs, blockSize, 10.f);
				drive.prepare(Fs, blockSize, 10.f);
				feedback.prepare(Fs, blockSize, 10.f);
				delayRate.prepare(Fs, blockSize, 10.f);
				gain.prepare(Fs, blockSize, 10.f);
				rmDepth.prepare(Fs, blockSize, 10.f);
				rmFreqHz.prepare(Fs, blockSize, 10.f);

				delayFB.prepare(delaySize);
				readHead.resize(blockSize, 0.f);

				delaySizeF = static_cast<float>(delaySize);
			}

			void operator()(float** samples, int numChannels, int numSamples,
				bool enabled, float _pitch, float _resonance, int _slope, float _drive, float _feedback,
				float _oct, float _semi, float _rmDepth, float _gain,
				const int* wHead, const XenManager& xen) noexcept
			{
				auto lane = laneBuffer.getArrayOfWritePointers();

				if (!enabled)
				{
					for (auto ch = 0; ch < numChannels; ++ch)
						SIMD::clear(lane[ch], numSamples);
					return;
				}

				for (auto ch = 0; ch < numChannels; ++ch)
					SIMD::copy(lane[ch], samples[ch], numSamples);

				const auto freqHz = xen.noteToFreqHzWithWrap(_pitch, 20.f);

				const auto fcBuf = frequency(freqHzInFc(freqHz, Fs), numSamples);
				const auto resoBuf = resonance(_resonance, numSamples);
				
				filter
				(
					lane, samples, numChannels, numSamples,
					fcBuf, resoBuf, _slope
				);
				
				const auto feedbackBuf = feedback(_feedback, numSamples);
				const auto delayPitch = _pitch + _oct * xen.getXen() + _semi;
				const auto delayFreqHz = xen.noteToFreqHzWithWrap(delayPitch, 5.f);
				const auto delaySamples = freqHzInSamples(delayFreqHz, Fs);
				const auto delayRateBuf = delayRate(delaySamples, numSamples);
				const auto rHead = getRHead(numSamples, wHead, delayRateBuf);
				delayFB(lane, numChannels, numSamples, wHead, rHead, feedbackBuf);

				const auto driveBuf = drive(_drive, numSamples);
				distort(lane, numChannels, numSamples, driveBuf);

				const auto rmDepthBuf = rmDepth(_rmDepth, numSamples);
				const auto rmFreqHzBuf = rmFreqHz(delayFreqHz, numSamples);
				ringMod(samples, numChannels, numSamples, rmDepthBuf, rmFreqHzBuf);

				const auto gainBuf = gain(decibelToGain(_gain), numSamples);
				applyGain(lane, numChannels, numSamples, gainBuf);
			}

			void addTo(float** samples, int numChannels, int numSamples) noexcept
			{
				auto lane = laneBuffer.getArrayOfReadPointers();

				for (auto ch = 0; ch < numChannels; ++ch)
					SIMD::add(samples[ch], lane[ch], numSamples);
			}

		protected:
			AudioBuffer laneBuffer;
			std::vector<float> readHead;
			Filter filter;
			PRM frequency, resonance, drive, feedback, delayRate, rmDepth, rmFreqHz, gain;
			DelayFeedback delayFB;
			RingMod ringMod;
			float Fs, delaySizeF;

			const float* getRHead(int numSamples, const int* wHead, const float* delayBuf) noexcept
			{
				auto rHead = readHead.data();
				for (auto s = 0; s < numSamples; ++s)
				{
					const auto w = static_cast<float>(wHead[s]);
					auto r = w - delayBuf[s];
					if (r < 0.f)
						r += delaySizeF;

					rHead[s] = r;
				}
				return rHead;
			}

			float distort(float x, float d) const noexcept
			{
				auto w = std::tanh(256.f * x) / 256.f;
				return x + d * (w - x);
			}

			void distort(float** samples, int numChannels, int numSamples, const float* driveBuf) noexcept
			{
				for (auto ch = 0; ch < numChannels; ++ch)
				{
					auto smpls = samples[ch];

					for (auto s = 0; s < numSamples; ++s)
						smpls[s] = distort(smpls[s], driveBuf[s]);
				}
			}

			void applyGain(float** samples, int numChannels, int numSamples, const float* gainBuf) noexcept
			{
				for (auto ch = 0; ch < numChannels; ++ch)
					SIMD::multiply(samples[ch], gainBuf, numSamples);
			}
		};

	public:
		Manta(const XenManager& _xen) :
			xen(_xen),
			lanes(),
			writeHead(),
			delaySize(1)
		{}

		void prepare(float sampleRate, int blockSize)
		{
			delaySize = static_cast<int>(std::ceil(freqHzInSamples(static_cast<float>(5.f), sampleRate)) + 3.f);
			if (delaySize % 2 != 0)
				++delaySize;

			for (auto& lane : lanes)
				lane.prepare(sampleRate, blockSize, delaySize);

			writeHead.prepare(blockSize, delaySize);
		}

		/* samples, numChannels, numSamples,
		* l1Enabled [0, 1], l1Pitch [12, N]note, l1Resonance [1, N]q, l1Slope [1, 4]db/oct, l1Drive [0, 1]%, l1Feedback [0, 1]%, l1Gain [-60, 60]db
		* l2Enabled [0, 1], l2Pitch [12, N]note, l2Resonance [1, N]q, l2Slope [1, 4]db/oct, l2Drive [0, 1]%, l2Feedback [0, 1]%, l2Gain [-60, 60]db
		* l3Enabled [0, 1], l3Pitch [12, N]note, l3Resonance [1, N]q, l3Slope [1, 4]db/oct, l3Drive [0, 1]%, l3Feedback [0, 1]%, l3Gain [-60, 60]db
		*/
		void operator()(float** samples, int numChannels, int numSamples,
			bool l1Enabled, float l1Pitch, float l1Resonance, int l1Slope, float l1Drive, float l1Feedback, float l1Oct, float l1Semi, float l1RMDepth, float l1Gain,
			bool l2Enabled, float l2Pitch, float l2Resonance, int l2Slope, float l2Drive, float l2Feedback, float l2Oct, float l2Semi, float l2RMDepth, float l2Gain,
			bool l3Enabled, float l3Pitch, float l3Resonance, int l3Slope, float l3Drive, float l3Feedback, float l3Oct, float l3Semi, float l3RMDepth, float l3Gain) noexcept
		{
			bool enabled[NumLanes] = { l1Enabled, l2Enabled, l3Enabled };
			float pitch[NumLanes] = { l1Pitch, l2Pitch, l3Pitch };
			float resonance[NumLanes] = { l1Resonance, l2Resonance, l3Resonance };
			int slope[NumLanes] = { l1Slope, l2Slope, l3Slope };
			float drive[NumLanes] = { l1Drive, l2Drive, l3Drive };
			float feedback[NumLanes] = { l1Feedback, l2Feedback, l3Feedback };
			float oct[NumLanes] = { l1Oct, l2Oct, l3Oct };
			float semi[NumLanes] = { l1Semi, l2Semi, l3Semi };
			float rmDepth[NumLanes] = { l1RMDepth, l2RMDepth, l3RMDepth };
			float gain[NumLanes] = { l1Gain, l2Gain, l3Gain };

			writeHead(numSamples);
			const auto wHead = writeHead.data();

			for (auto i = 0; i < NumLanes; ++i)
			{
				auto& lane = lanes[i];

				lane
				(
					samples,
					numChannels,
					numSamples,
					
					enabled[i],
					pitch[i],
					resonance[i],
					slope[i],
					drive[i],
					feedback[i],
					oct[i],
					semi[i],
					rmDepth[i],
					gain[i],
					
					wHead,
					xen
				);
			}
			
			for (auto ch = 0; ch < numChannels; ++ch)
				SIMD::clear(samples[ch], numSamples);
			for (auto& lane : lanes)
				lane.addTo(samples, numChannels, numSamples);
		}
		
	protected:
		const XenManager& xen;
		std::array<Lane, NumLanes> lanes;
		WHead writeHead;
	public:
		int delaySize;
	};
}

/*

todo:


*/