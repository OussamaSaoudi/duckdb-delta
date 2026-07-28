//===----------------------------------------------------------------------===//
// delta_kernel.hpp — engine-neutral C++ SDK for Delta Kernel scan plans.
//
// This is intentionally a real information-hiding boundary: the generated C ABI header and all
// raw kernel handles live in delta_kernel.cpp. Consumers see C++ values, protobuf plans, and the
// standard Arrow C Data layout only.
//===----------------------------------------------------------------------===//
#pragma once

#include "plan.pb.h"
#include "schema.pb.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace delta {

namespace plan = ::delta::kernel::plan;
namespace schema = ::delta::kernel::schema;

class DeltaError : public std::runtime_error {
public:
	explicit DeltaError(const std::string &message) : std::runtime_error(message) {
	}
};

struct EngineError {
	std::string message;
};

enum class ScanKind { Metadata, Data };
enum class Comparison { LessThan, LessThanOrEqual, GreaterThan, GreaterThanOrEqual, Equal, NotEqual };

// Arrow C Data Interface layouts. These are an external interchange ABI, not Delta Kernel's C ABI.
struct ArrowArray {
	int64_t length = 0;
	int64_t null_count = 0;
	int64_t offset = 0;
	int64_t n_buffers = 0;
	int64_t n_children = 0;
	const void **buffers = nullptr;
	ArrowArray **children = nullptr;
	ArrowArray *dictionary = nullptr;
	void (*release)(ArrowArray *) = nullptr;
	void *private_data = nullptr;
};

struct ArrowSchema {
	const char *format = nullptr;
	const char *name = nullptr;
	const char *metadata = nullptr;
	int64_t flags = 0;
	int64_t n_children = 0;
	ArrowSchema **children = nullptr;
	ArrowSchema *dictionary = nullptr;
	void (*release)(ArrowSchema *) = nullptr;
	void *private_data = nullptr;
};

class ArrowBatch {
public:
	ArrowBatch() = default;
	~ArrowBatch();
	ArrowBatch(ArrowBatch &&other) noexcept;
	ArrowBatch &operator=(ArrowBatch &&other) noexcept;
	ArrowBatch(const ArrowBatch &) = delete;
	ArrowBatch &operator=(const ArrowBatch &) = delete;

	ArrowArray *mutable_array() noexcept {
		return &array_;
	}
	ArrowSchema *mutable_schema() noexcept {
		return &schema_;
	}

private:
	friend class Reducer;
	void reset() noexcept;
	ArrowArray array_ {};
	ArrowSchema schema_ {};
};

using ArrowStream = std::function<std::optional<ArrowBatch>()>;
enum class Control { Continue, Break };

// Immutable SDK expression values used to build a data-skipping predicate. Their representation
// and the C visitor bridge are private to the SDK implementation.
class Expression {
public:
	Expression(const Expression &) = default;
	Expression(Expression &&) noexcept = default;
	Expression &operator=(const Expression &) = default;
	Expression &operator=(Expression &&) noexcept = default;
	~Expression() = default;

	static Expression column(std::string name);
	static Expression boolean(bool value);
	static Expression int8(int8_t value);
	static Expression int16(int16_t value);
	static Expression int32(int32_t value);
	static Expression int64(int64_t value);
	static Expression float32(float value);
	static Expression float64(double value);
	static Expression string(std::string value);
	static Expression date(int32_t days_since_epoch);
	static Expression decimal(uint64_t high_bits, uint64_t low_bits, uint8_t precision, uint8_t scale);

private:
	struct Impl;
	explicit Expression(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {
	}
	std::shared_ptr<const Impl> impl_;
	friend class Predicate;
	friend struct PredicateBridge;
};

class Predicate {
public:
	Predicate(const Predicate &) = default;
	Predicate(Predicate &&) noexcept = default;
	Predicate &operator=(const Predicate &) = default;
	Predicate &operator=(Predicate &&) noexcept = default;
	~Predicate() = default;

	static Predicate compare(Comparison comparison, Expression left, Expression right);
	static Predicate all(std::vector<Predicate> children);
	static Predicate any(std::vector<Predicate> children);
	static Predicate logical_not(Predicate child);
	static Predicate is_null(Expression expression);

private:
	struct Impl;
	explicit Predicate(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {
	}
	std::shared_ptr<const Impl> impl_;
	friend class Snapshot;
	friend struct PredicateBridge;
};

class FinishedReducer {
public:
	~FinishedReducer();
	FinishedReducer(FinishedReducer &&) noexcept;
	FinishedReducer &operator=(FinishedReducer &&) noexcept;
	FinishedReducer(const FinishedReducer &) = delete;
	FinishedReducer &operator=(const FinishedReducer &) = delete;

private:
	struct Impl;
	explicit FinishedReducer(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend class Reducer;
	friend class SnapshotStateMachine;
	friend class ScanStateMachine;
	friend struct SdkBridge;
};

class Reducer {
public:
	~Reducer();
	Reducer(Reducer &&) noexcept;
	Reducer &operator=(Reducer &&) noexcept;
	Reducer(const Reducer &) = delete;
	Reducer &operator=(const Reducer &) = delete;

	Control apply(ArrowBatch &batch);
	FinishedReducer finish() &&;
	FinishedReducer apply_all(ArrowStream stream) &&;

private:
	struct Impl;
	explicit Reducer(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend class SnapshotStateMachine;
	friend class ScanStateMachine;
	friend struct SdkBridge;
};

struct Reduce {
	plan::ResultPlan plan;
	Reducer reducer;
};
struct Done {};
using EngineRequest = std::variant<Reduce, Done>;

class Engine {
public:
	virtual ~Engine() = default;
	virtual ArrowStream execute_to_arrow(const plan::ResultPlan &plan) = 0;
};

class Snapshot;
class Scan;

class SnapshotStateMachine {
public:
	~SnapshotStateMachine();
	SnapshotStateMachine(SnapshotStateMachine &&) noexcept;
	SnapshotStateMachine &operator=(SnapshotStateMachine &&) noexcept;
	SnapshotStateMachine(const SnapshotStateMachine &) = delete;
	SnapshotStateMachine &operator=(const SnapshotStateMachine &) = delete;

	EngineRequest next();
	void submit(FinishedReducer result);
	[[noreturn]] void submit(const EngineError &error);
	Snapshot build() &&;

private:
	struct Impl;
	explicit SnapshotStateMachine(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend SnapshotStateMachine open_snapshot(const std::string &, int64_t);
	friend struct SdkBridge;
};

class ScanStateMachine {
public:
	~ScanStateMachine();
	ScanStateMachine(ScanStateMachine &&) noexcept;
	ScanStateMachine &operator=(ScanStateMachine &&) noexcept;
	ScanStateMachine(const ScanStateMachine &) = delete;
	ScanStateMachine &operator=(const ScanStateMachine &) = delete;

	EngineRequest next();
	void submit(FinishedReducer result);
	[[noreturn]] void submit(const EngineError &error);
	Scan build() &&;

private:
	struct Impl;
	explicit ScanStateMachine(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend class Snapshot;
	friend struct SdkBridge;
};

class Snapshot {
public:
	~Snapshot();
	Snapshot(Snapshot &&) noexcept;
	Snapshot &operator=(Snapshot &&) noexcept;
	Snapshot(const Snapshot &) = delete;
	Snapshot &operator=(const Snapshot &) = delete;

	int64_t version() const noexcept;
	const schema::StructType &schema() const;
	ScanStateMachine scan_sm(ScanKind kind, const Predicate *predicate = nullptr) const;
	Scan scan(Engine &engine, ScanKind kind, const Predicate *predicate = nullptr) const;

private:
	struct Impl;
	explicit Snapshot(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend class SnapshotStateMachine;
};

class Scan {
public:
	~Scan();
	Scan(Scan &&) noexcept;
	Scan &operator=(Scan &&) noexcept;
	Scan(const Scan &) = delete;
	Scan &operator=(const Scan &) = delete;

	const plan::ResultPlan &plan() const noexcept;

private:
	struct Impl;
	explicit Scan(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend class ScanStateMachine;
};

SnapshotStateMachine open_snapshot(const std::string &path, int64_t version = -1);
Snapshot drive(SnapshotStateMachine state_machine, Engine &engine);
Scan drive(ScanStateMachine state_machine, Engine &engine);

} // namespace delta
