#pragma once

#include <cassert>
#include <cstring>
#include <memory>

#include <CL/sycl.hpp>

#include "ranges.h"
#include "workaround.h"

namespace celerity {
namespace detail {

	struct raw_data_handle {
		void* linearized_data_ptr = nullptr;
		cl::sycl::range<3> range;
		cl::sycl::id<3> offset;

		raw_data_handle() = default;
		raw_data_handle(void* linearized_data_ptr, cl::sycl::range<3> range, cl::sycl::id<3> offset)
		    : linearized_data_ptr(linearized_data_ptr), range(range), offset(offset) {}
		virtual ~raw_data_handle() = default;
	};

	struct raw_data_read_handle : raw_data_handle {
		size_t linearized_data_size = 0;

		void allocate(size_t byte_size) {
			linearized_data_ptr = malloc(byte_size);
			own_data = true;
		}

		~raw_data_read_handle() {
			if(linearized_data_ptr != nullptr && own_data) { free(linearized_data_ptr); }
		}

	  private:
		bool own_data = false;
	};

	enum class buffer_type { HOST_BUFFER, DEVICE_BUFFER };

	class buffer_storage_base {
	  public:
		/**
		 * @param type The type of buffer. This is kind of a hack for now, to avoid host buffers used in master-access tasks
		 * clogging up device memory for no reason.
		 * TODO: Is there any scenario where we'd want to change this over time?
		 * TODO: Consider making host / device buffers different (templatized) types entirely
		 * @param range The size of the buffer
		 */
		buffer_storage_base(buffer_type type, cl::sycl::range<3> range) : type(type), range(range) {}

		buffer_type get_type() const { return type; }

		cl::sycl::range<3> get_range() const { return range; }

		virtual std::shared_ptr<raw_data_read_handle> get_data(cl::sycl::queue& queue, const cl::sycl::id<3>& offset, const cl::sycl::range<3>& range) = 0;
		virtual void set_data(cl::sycl::queue& queue, const raw_data_handle& dh) = 0;
		virtual ~buffer_storage_base() = default;

	  private:
		buffer_type type = buffer_type::HOST_BUFFER;
		cl::sycl::range<3> range;
	};

	inline size_t get_linear_index(const cl::sycl::range<1>& buffer_range, const cl::sycl::id<1>& index) { return index[0]; }

	inline size_t get_linear_index(const cl::sycl::range<2>& buffer_range, const cl::sycl::id<2>& index) { return index[0] * buffer_range[1] + index[1]; }

	inline size_t get_linear_index(const cl::sycl::range<3>& buffer_range, const cl::sycl::id<3>& index) {
		return index[0] * buffer_range[1] * buffer_range[2] + index[1] * buffer_range[2] + index[2];
	}

	template <typename DataT>
	void copy_buffer(const DataT* source_base_ptr, DataT* target_base_ptr, const cl::sycl::range<1> source_range, const cl::sycl::id<1> source_offset,
	    const cl::sycl::range<1> target_range, const cl::sycl::id<1> target_offset, const cl::sycl::range<1>& copy_range) {
		const size_t line_size = sizeof(DataT) * copy_range[0];
		std::memcpy(
		    target_base_ptr + get_linear_index(target_range, target_offset), source_base_ptr + get_linear_index(source_range, source_offset), line_size);
	}

	template <typename DataT>
	void copy_buffer(const DataT* source_base_ptr, DataT* target_base_ptr, const cl::sycl::range<2> source_range, const cl::sycl::id<2> source_offset,
	    const cl::sycl::range<2> target_range, const cl::sycl::id<2> target_offset, const cl::sycl::range<2>& copy_range) {
		const size_t line_size = sizeof(DataT) * copy_range[1];
		const auto source_base_offset = get_linear_index(source_range, source_offset);
		const auto target_base_offset = get_linear_index(target_range, target_offset);
		for(size_t i = 0; i < copy_range[0]; ++i) {
			std::memcpy(target_base_ptr + target_base_offset + i * target_range[1], source_base_ptr + source_base_offset + i * source_range[1], line_size);
		}
	}

	template <typename DataT>
	void copy_buffer(const DataT* source_base_ptr, DataT* target_base_ptr, const cl::sycl::range<3> source_range, const cl::sycl::id<3> source_offset,
	    const cl::sycl::range<3> target_range, const cl::sycl::id<3> target_offset, const cl::sycl::range<3>& copy_range) {
		throw std::runtime_error("3D buffer copying NYI");
	}

	// FIXME: Remove this
	template <typename DataT, int Dims>
	class computecpp_get_data_workaround {};
	template <typename DataT, int Dims>
	class computecpp_set_data_workaround {};

	template <typename DataT, int Dims>
	class buffer_storage : public virtual buffer_storage_base {
	  public:
		buffer_storage(buffer_type type, cl::sycl::range<Dims> range) : buffer_storage_base(type, detail::range_cast<3>(range)) {
			// Initialize device buffers eagerly, as they will very likely be required later anyway.
			// (For host buffers it makes sense to initialize lazily, as typically only a few "result buffers" will be used in master-access tasks).
			if(type == buffer_type::DEVICE_BUFFER) { sycl_buf = std::make_unique<cl::sycl::buffer<DataT, Dims>>(detail::range_cast<Dims>(get_range())); }
		}

		/**
		 * @brief Returns the underlying SYCL buffer.
		 * Only available for DEVICE_BUFFERs.
		 */
		cl::sycl::buffer<DataT, Dims>& get_sycl_buffer() {
			assert(get_type() == buffer_type::DEVICE_BUFFER && "Trying to access SYCL buffer on HOST_BUFFER");
			return *sycl_buf;
		}

		std::shared_ptr<raw_data_read_handle> get_data(cl::sycl::queue& queue, const cl::sycl::id<3>& offset, const cl::sycl::range<3>& range) override {
			assert(Dims > 1 || (offset[1] == 0 && range[1] == 1));
			assert(Dims > 2 || (offset[2] == 0 && range[2] == 1));

			auto result = std::make_shared<raw_data_read_handle>();
			result->range = range;
			result->offset = offset;
			result->linearized_data_size = sizeof(DataT) * range[0] * range[1] * range[2];

			if(get_type() == buffer_type::DEVICE_BUFFER) {
				result->allocate(result->linearized_data_size);
				// TODO: Ideally we'd not wait here and instead return some sort of async handle that can be waited upon
				auto buf = get_sycl_buffer();
				// Explicit memory operations appear to be broken in ComputeCpp as of version 1.0.5
				// As a workaround we create a temporary buffer and copy the contents manually.
#if WORKAROUND(COMPUTECPP, 1, 0, 5)
				cl::sycl::buffer<DataT, Dims> tmp_dst_buf(reinterpret_cast<DataT*>(result->linearized_data_ptr), cl::sycl::range<Dims>(range));
				const auto dim_offset = cl::sycl::id<Dims>(offset);
				auto event = queue.submit([&](cl::sycl::handler& cgh) {
					auto src_acc = buf.template get_access<cl::sycl::access::mode::read>(cgh, cl::sycl::range<Dims>(range), dim_offset);
					auto dst_acc = tmp_dst_buf.template get_access<cl::sycl::access::mode::discard_write>(cgh);
					cgh.parallel_for<computecpp_get_data_workaround<DataT, Dims>>(
					    cl::sycl::range<Dims>(range), [=](cl::sycl::item<Dims> item) { dst_acc[item] = src_acc[item.get_id() + dim_offset]; });
				});
#else
				auto event = queue.submit([&](cl::sycl::handler& cgh) {
					auto acc = buf.template get_access<cl::sycl::access::mode::read>(cgh, detail::range_cast<Dims>(range), detail::id_cast<Dims>(offset));
					cgh.copy(acc, reinterpret_cast<DataT*>(result->linearized_data_ptr));
				});
#endif
				event.wait();
			} else {
				// FIXME: It's kind of bad that accessing the full HOST_BUFFER is much cheaper than accessing a subrange.
				// At least for the celerity::host_accessor we could work around this by computing the proper linear offsets internally on the fly
				if(range == get_range() && offset == cl::sycl::id<3>{}) {
					result->linearized_data_ptr = get_host_pointer();
				} else {
					result->allocate(result->linearized_data_size);
					copy_buffer(get_host_pointer(), reinterpret_cast<DataT*>(result->linearized_data_ptr), detail::range_cast<Dims>(get_range()),
					    detail::id_cast<Dims>(offset), detail::range_cast<Dims>(range), cl::sycl::id<Dims>{}, detail::range_cast<Dims>(range));
				}
			}

			return result;
		}

		void set_data(cl::sycl::queue& queue, const raw_data_handle& dh) override {
			if(get_type() == buffer_type::DEVICE_BUFFER) {
				// TODO: Ideally we'd not wait here and instead return some sort of async handle that can be waited upon
				auto buf = get_sycl_buffer();
				// Explicit memory operations appear to be broken in ComputeCpp as of version 1.0.5
				// As a workaround we create a temporary buffer and copy the contents manually.
#if WORKAROUND(COMPUTECPP, 1, 0, 5)
				cl::sycl::buffer<DataT, Dims> tmp_src_buf(reinterpret_cast<DataT*>(dh.linearized_data_ptr), cl::sycl::range<Dims>(dh.range));
				const auto dim_offset = cl::sycl::id<Dims>(dh.offset);
				auto event = queue.submit([&](cl::sycl::handler& cgh) {
					auto src_acc = tmp_src_buf.template get_access<cl::sycl::access::mode::read>(cgh);
					auto dst_acc = buf.template get_access<cl::sycl::access::mode::discard_write>(cgh, cl::sycl::range<Dims>(dh.range), dim_offset);
					cgh.parallel_for<computecpp_set_data_workaround<DataT, Dims>>(
					    cl::sycl::range<Dims>(dh.range), [=](cl::sycl::item<Dims> item) { dst_acc[item.get_id() + dim_offset] = src_acc[item]; });
				});
#else
				auto event = queue.submit([&](cl::sycl::handler& cgh) {
					auto acc = buf.template get_access<cl::sycl::access::mode::discard_write>(
					    cgh, detail::range_cast<Dims>(dh.range), detail::id_cast<Dims>(dh.offset));
					cgh.copy(reinterpret_cast<DataT*>(dh.linearized_data_ptr), acc);
				});
#endif
				event.wait();
			} else {
				if(dh.range == get_range() && dh.offset == cl::sycl::id<3>{}) {
					// Copy full buffer
					auto host_ptr = get_host_pointer();
					const size_t data_size = sizeof(DataT) * dh.range[0] * dh.range[1] * dh.range[2];
					std::memcpy(host_ptr, reinterpret_cast<DataT*>(dh.linearized_data_ptr), data_size);
				} else {
					// Copy 1D/2D/3D rect
					copy_buffer(reinterpret_cast<DataT*>(dh.linearized_data_ptr), get_host_pointer(), detail::range_cast<Dims>(dh.range), cl::sycl::id<Dims>{},
					    detail::range_cast<Dims>(get_range()), detail::id_cast<Dims>(dh.offset), detail::range_cast<Dims>(dh.range));
				}
			}
		}

	  private:
		// Only used for DEVICE_BUFFERs
		std::unique_ptr<cl::sycl::buffer<DataT, Dims>> sycl_buf;

		// Only used for HOST_BUFFERs
		std::vector<DataT> lazy_host_buf;

		/**
		 * @brief Returns a pointer to the underlying lazy host buffer. If the buffer doesn't exist yet, it is created.
		 * Only available for HOST_BUFFERs.
		 */
		DataT* get_host_pointer() {
			assert(get_type() == buffer_type::HOST_BUFFER && "Trying to access host buffer on DEVICE_BUFFER");
			auto range = get_range();
			if(lazy_host_buf.empty()) { lazy_host_buf.resize(range[0] * range[1] * range[2]); }
			return lazy_host_buf.data();
		}
	};

} // namespace detail
} // namespace celerity
