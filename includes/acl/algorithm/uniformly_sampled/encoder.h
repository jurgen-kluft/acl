#pragma once

////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2017 Nicholas Frechette & Animation Compression Library contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "acl/core/memory.h"
#include "acl/core/error.h"
#include "acl/core/bitset.h"
#include "acl/core/algorithm_globals.h"
#include "acl/core/utils.h"
#include "acl/algorithm/uniformly_sampled/common.h"
#include "acl/compression/compressed_clip_impl.h"
#include "acl/compression/skeleton.h"
#include "acl/compression/animation_clip.h"
#include "acl/math/quat_32.h"
#include "acl/math/vector4_32.h"

#include <stdint.h>

//////////////////////////////////////////////////////////////////////////
// Full Precision Encoder
//
// The goal of the full precision format is to be used as a reference
// point for compression speed, compressed size, and decompression speed.
// This will not be a raw format in that we will at least drop constant
// or bind pose tracks. As such, it is near-raw but not quite.
//
// This is the highest precision encoder and the fastest to compress.
//
// Data format:
//    TODO: Detail the format
//////////////////////////////////////////////////////////////////////////

namespace acl
{
	namespace uniformly_sampled
	{
		namespace impl
		{
			inline void get_num_animated_tracks(const AnimationClip& clip, uint32_t& out_num_constant_rotation_tracks, uint32_t& out_num_constant_translation_tracks,
												uint32_t& out_num_animated_rotation_tracks, uint32_t& out_num_animated_translation_tracks)
			{
				uint16_t num_bones = clip.get_num_bones();

				uint32_t num_constant_rotation_tracks = 0;
				uint32_t num_constant_translation_tracks = 0;
				uint32_t num_animated_rotation_tracks = 0;
				uint32_t num_animated_translation_tracks = 0;

				for (uint16_t bone_index = 0; bone_index < num_bones; ++bone_index)
				{
					const AnimatedBone& bone = clip.get_animated_bone(bone_index);

					if (!bone.rotation_track.is_default())
					{
						if (bone.rotation_track.is_constant())
							num_constant_rotation_tracks++;
						else
							num_animated_rotation_tracks++;
					}

					if (!bone.translation_track.is_default())
					{
						if (bone.translation_track.is_constant())
							num_constant_translation_tracks++;
						else
							num_animated_translation_tracks++;
					}
				}

				out_num_constant_rotation_tracks = num_constant_rotation_tracks;
				out_num_constant_translation_tracks = num_constant_translation_tracks;
				out_num_animated_rotation_tracks = num_animated_rotation_tracks;
				out_num_animated_translation_tracks = num_animated_translation_tracks;
			}

			inline void write_default_track_bitset(FullPrecisionHeader& header, const AnimationClip& clip, uint32_t bitset_size)
			{
				uint32_t* default_tracks_bitset = header.get_default_tracks_bitset();
				uint32_t default_track_offset = 0;

				bitset_reset(default_tracks_bitset, bitset_size, false);

				for (uint16_t bone_index = 0; bone_index < header.num_bones; ++bone_index)
				{
					const AnimatedBone& bone = clip.get_animated_bone(bone_index);

					bool is_rotation_default = bone.rotation_track.is_default();
					bool is_translation_default = bone.translation_track.is_default();

					bitset_set(default_tracks_bitset, bitset_size, default_track_offset++, is_rotation_default);
					bitset_set(default_tracks_bitset, bitset_size, default_track_offset++, is_translation_default);
				}
			}

			inline void write_constant_track_bitset(FullPrecisionHeader& header, const AnimationClip& clip, uint32_t bitset_size)
			{
				uint32_t* constant_tracks_bitset = header.get_constant_tracks_bitset();
				uint32_t constant_track_offset = 0;

				bitset_reset(constant_tracks_bitset, bitset_size, false);

				for (uint16_t bone_index = 0; bone_index < header.num_bones; ++bone_index)
				{
					const AnimatedBone& bone = clip.get_animated_bone(bone_index);

					bool is_rotation_constant = bone.rotation_track.is_constant();
					bool is_translation_constant = bone.translation_track.is_constant();

					bitset_set(constant_tracks_bitset, bitset_size, constant_track_offset++, is_rotation_constant);
					bitset_set(constant_tracks_bitset, bitset_size, constant_track_offset++, is_translation_constant);
				}
			}

			inline size_t quantize_unsigned_normalized(float input, size_t num_bits)
			{
				ACL_ENSURE(input >= 0.0f && input <= 1.0f, "Invalue input value: 0.0 <= %f <= 1.0", input);
				size_t max_value = (1 << num_bits) - 1;
				return static_cast<size_t>(symmetric_round(input * safe_to_float(max_value)));
			}

			inline size_t quantize_signed_normalized(float input, size_t num_bits)
			{
				ACL_ENSURE(input >= -1.0f && input <= 1.0f, "Invalue input value: -1.0 <= %f <= 1.0", input);
				return quantize_unsigned_normalized((input * 0.5f) + 0.5f, num_bits);
			}

			inline void write_rotation(const FullPrecisionHeader& header, const Quat_32& rotation, uint8_t*& out_rotation_data)
			{
				if (header.rotation_format == RotationFormat8::Quat_128)
				{
					quat_unaligned_write(rotation, out_rotation_data);
				}
				else if (header.rotation_format == RotationFormat8::Quat_96)
				{
					Vector4_32 rotation_xyz = quat_to_vector(quat_ensure_positive_w(rotation));
					vector_unaligned_write3(rotation_xyz, out_rotation_data);
				}
				else if (header.rotation_format == RotationFormat8::Quat_48)
				{
					Vector4_32 rotation_xyz = quat_to_vector(quat_ensure_positive_w(rotation));

					size_t rotation_x = quantize_signed_normalized(vector_get_x(rotation_xyz), 16);
					size_t rotation_y = quantize_signed_normalized(vector_get_y(rotation_xyz), 16);
					size_t rotation_z = quantize_signed_normalized(vector_get_z(rotation_xyz), 16);

					uint16_t* data = safe_ptr_cast<uint16_t>(out_rotation_data);
					data[0] = safe_static_cast<uint16_t>(rotation_x);
					data[1] = safe_static_cast<uint16_t>(rotation_y);
					data[2] = safe_static_cast<uint16_t>(rotation_z);
				}
				else if (header.rotation_format == RotationFormat8::Quat_32)
				{
					Vector4_32 rotation_xyz = quat_to_vector(quat_ensure_positive_w(rotation));

					size_t rotation_x = quantize_signed_normalized(vector_get_x(rotation_xyz), 11);
					size_t rotation_y = quantize_signed_normalized(vector_get_y(rotation_xyz), 11);
					size_t rotation_z = quantize_signed_normalized(vector_get_z(rotation_xyz), 10);

					uint32_t rotation_u32 = safe_static_cast<uint32_t>((rotation_x << 21) | (rotation_y << 10) | rotation_z);
					
					// Written 2 bytes at a time to ensure safe alignment
					uint16_t* data = safe_ptr_cast<uint16_t>(out_rotation_data);
					data[0] = safe_static_cast<uint16_t>(rotation_u32 >> 16);
					data[1] = safe_static_cast<uint16_t>(rotation_u32 & 0xFFFF);
				}

				out_rotation_data += get_rotation_size(header.rotation_format);
			}

			inline void write_translation(const FullPrecisionHeader& header, const Vector4_32& translation, uint8_t*& out_translation_data)
			{
				if (header.translation_format == VectorFormat8::Vector3_96)
				{
					vector_unaligned_write3(translation, out_translation_data);
				}
				else if (header.translation_format == VectorFormat8::Vector3_48)
				{
					size_t translation_x = quantize_signed_normalized(vector_get_x(translation), 16);
					size_t translation_y = quantize_signed_normalized(vector_get_y(translation), 16);
					size_t translation_z = quantize_signed_normalized(vector_get_z(translation), 16);

					uint16_t* data = safe_ptr_cast<uint16_t>(out_translation_data);
					data[0] = safe_static_cast<uint16_t>(translation_x);
					data[1] = safe_static_cast<uint16_t>(translation_y);
					data[2] = safe_static_cast<uint16_t>(translation_z);
				}

				out_translation_data += get_translation_size(header.translation_format);
			}

			inline void write_constant_track_data(FullPrecisionHeader& header, const AnimationClip& clip, uint32_t constant_data_size)
			{
				uint8_t* constant_data = header.get_constant_track_data();
				const uint8_t* constant_data_end = add_offset_to_ptr<uint8_t>(constant_data, constant_data_size);

				bool are_translations_always_aligned = header.rotation_format != RotationFormat8::Quat_48;

				for (uint16_t bone_index = 0; bone_index < header.num_bones; ++bone_index)
				{
					const AnimatedBone& bone = clip.get_animated_bone(bone_index);

					if (!bone.rotation_track.is_default() && bone.rotation_track.is_constant())
					{
						Quat_32 rotation = quat_cast(bone.rotation_track.get_sample(0));
						write_rotation(header, rotation, constant_data);
					}

					if (!bone.translation_track.is_default() && bone.translation_track.is_constant())
					{
						Vector4_32 translation = vector_cast(bone.translation_track.get_sample(0));
						write_translation(header, translation, constant_data);
					}

					ACL_ENSURE(constant_data <= constant_data_end, "Invalid constant data offset. Wrote too much data.");
				}

				ACL_ENSURE(constant_data == constant_data_end, "Invalid constant data offset. Wrote too little data.");
			}

			inline void write_animated_track_data(FullPrecisionHeader& header, const AnimationClip& clip, uint32_t animated_data_size)
			{
				uint8_t* animated_track_data = header.get_track_data();
				const uint8_t* animated_track_data_end = add_offset_to_ptr<uint8_t>(animated_track_data, animated_data_size);

				// Data is sorted first by time, second by bone.
				// This ensures that all bones are contiguous in memory when we sample a particular time.
				for (uint32_t sample_index = 0; sample_index < header.num_samples; ++sample_index)
				{
					for (uint16_t bone_index = 0; bone_index < header.num_bones; ++bone_index)
					{
						const AnimatedBone& bone = clip.get_animated_bone(bone_index);

						if (bone.rotation_track.is_animated())
						{
							Quat_32 rotation = quat_cast(bone.rotation_track.get_sample(sample_index));
							write_rotation(header, rotation, animated_track_data);
						}

						if (bone.translation_track.is_animated())
						{
							Vector4_32 translation = vector_cast(bone.translation_track.get_sample(sample_index));
							write_translation(header, translation, animated_track_data);
						}

						ACL_ENSURE(animated_track_data <= animated_track_data_end, "Invalid animated track data offset. Wrote too much data.");
					}
				}

				ACL_ENSURE(animated_track_data == animated_track_data_end, "Invalid animated track data offset. Wrote too little data.");
			}
		}

		// Encoder entry point
		inline CompressedClip* compress_clip(Allocator& allocator, const AnimationClip& clip, const RigidSkeleton& skeleton, RotationFormat8 rotation_format, VectorFormat8 translation_format)
		{
			using namespace impl;

			uint16_t num_bones = clip.get_num_bones();
			uint32_t num_samples = clip.get_num_samples();

			uint32_t num_constant_rotation_tracks;
			uint32_t num_constant_translation_tracks;
			uint32_t num_animated_rotation_tracks;
			uint32_t num_animated_translation_tracks;
			get_num_animated_tracks(clip, num_constant_rotation_tracks, num_constant_translation_tracks, num_animated_rotation_tracks, num_animated_translation_tracks);

			uint32_t rotation_size = get_rotation_size(rotation_format);
			uint32_t translation_size = get_translation_size(translation_format);

			uint32_t constant_data_size = (rotation_size * num_constant_rotation_tracks) + (translation_size * num_constant_translation_tracks);
			uint32_t animated_data_size = (rotation_size * num_animated_rotation_tracks) + (translation_size * num_animated_translation_tracks);

			animated_data_size *= num_samples;

			uint32_t bitset_size = ((num_bones * FullPrecisionConstants::NUM_TRACKS_PER_BONE) + FullPrecisionConstants::BITSET_WIDTH - 1) / FullPrecisionConstants::BITSET_WIDTH;

			uint32_t buffer_size = 0;
			buffer_size += sizeof(CompressedClip);
			buffer_size += sizeof(FullPrecisionHeader);
			buffer_size += sizeof(uint32_t) * bitset_size;		// Default tracks bitset
			buffer_size += sizeof(uint32_t) * bitset_size;		// Constant tracks bitset
			buffer_size += constant_data_size;					// Constant track data
			buffer_size = align_to(buffer_size, 4);				// Align animated data
			buffer_size += animated_data_size;					// Animated track data

			uint8_t* buffer = allocate_type_array<uint8_t>(allocator, buffer_size, 16);

			CompressedClip* compressed_clip = make_compressed_clip(buffer, buffer_size, AlgorithmType8::UniformlySampled);

			FullPrecisionHeader& header = get_full_precision_header(*compressed_clip);
			header.num_bones = num_bones;
			header.rotation_format = rotation_format;
			header.translation_format = translation_format;
			header.num_samples = num_samples;
			header.sample_rate = clip.get_sample_rate();
			header.num_animated_rotation_tracks = num_animated_rotation_tracks;
			header.num_animated_translation_tracks = num_animated_translation_tracks;
			header.default_tracks_bitset_offset = sizeof(FullPrecisionHeader);
			header.constant_tracks_bitset_offset = header.default_tracks_bitset_offset + (sizeof(uint32_t) * bitset_size);
			header.constant_track_data_offset = header.constant_tracks_bitset_offset + (sizeof(uint32_t) * bitset_size);	// Aligned to 4 bytes
			header.track_data_offset = align_to(header.constant_track_data_offset + constant_data_size, 4);					// Aligned to 4 bytes

			write_default_track_bitset(header, clip, bitset_size);
			write_constant_track_bitset(header, clip, bitset_size);
			write_constant_track_data(header, clip, constant_data_size);
			write_animated_track_data(header, clip, animated_data_size);

			finalize_compressed_clip(*compressed_clip);

			return compressed_clip;
		}
	}
}