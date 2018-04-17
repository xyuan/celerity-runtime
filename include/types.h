#pragma once

#include <boost/variant/variant.hpp>

namespace celerity {

template <typename T, typename UniqueName>
class PhantomType {
  public:
	PhantomType() = default;
	PhantomType(T const& value) : value(value) {}
	PhantomType(T&& value) : value(std::move(value)) {}

	// Allow implicit conversion to underlying type, otherwise it becomes too annoying to use.
	// Luckily compilers won't do more than one user-defined conversion, so something like
	// PhantomType1<T> -> T -> PhantomType2<T>, can't happen. Therefore we still retain
	// strong typesafety between phantom types with the same underlying type.
	operator T() const { return value; }

  private:
	T value;
};

} // namespace celerity

#define MAKE_PHANTOM_TYPE(TypeName, UnderlyingT)                                                                                                               \
	namespace celerity {                                                                                                                                       \
		using TypeName = PhantomType<UnderlyingT, class TypeName##_PhantomType>;                                                                               \
	}                                                                                                                                                          \
	namespace std {                                                                                                                                            \
		template <>                                                                                                                                            \
		struct hash<celerity::TypeName> {                                                                                                                      \
			std::size_t operator()(const celerity::TypeName& t) const noexcept { return std::hash<UnderlyingT>{}(static_cast<const UnderlyingT>(t)); }         \
		};                                                                                                                                                     \
	}

MAKE_PHANTOM_TYPE(task_id, size_t)
MAKE_PHANTOM_TYPE(vertex, size_t)
MAKE_PHANTOM_TYPE(buffer_id, size_t)
MAKE_PHANTOM_TYPE(node_id, size_t)

namespace celerity {

using any_range = boost::variant<cl::sycl::range<1>, cl::sycl::range<2>, cl::sycl::range<3>>;

} // namespace celerity