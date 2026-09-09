#include "crossSimAnalogBackend.h"

#include <Python.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace SST {
namespace Mittens {

namespace {

class GilGuard
{
  public:
    GilGuard()
    {
        if (!Py_IsInitialized()) {
            Py_Initialize();
        }
        state_ = PyGILState_Ensure();
    }

    ~GilGuard()
    {
        PyGILState_Release(state_);
    }

    GilGuard(const GilGuard&) = delete;
    GilGuard& operator=(const GilGuard&) = delete;

  private:
    PyGILState_STATE state_;
};

class PyRef
{
  public:
    explicit PyRef(PyObject* object = nullptr) : object_(object) {}
    ~PyRef() { Py_XDECREF(object_); }

    PyRef(const PyRef&) = delete;
    PyRef& operator=(const PyRef&) = delete;

    PyRef(PyRef&& other) noexcept : object_(other.release()) {}

    PyRef& operator=(PyRef&& other) noexcept
    {
        if (this != &other) {
            Py_XDECREF(object_);
            object_ = other.release();
        }
        return *this;
    }

    PyObject* get() const noexcept { return object_; }

    PyObject* release() noexcept
    {
        PyObject* object = object_;
        object_ = nullptr;
        return object;
    }

  private:
    PyObject* object_;
};

std::string consumePythonError(const std::string& context)
{
    if (!PyErr_Occurred()) {
        return context;
    }

    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);

    PyRef ownedType(type);
    PyRef ownedValue(value);
    PyRef ownedTraceback(traceback);
    PyRef rendered(value == nullptr ? nullptr : PyObject_Str(value));

    if (rendered.get() == nullptr) {
        PyErr_Clear();
        return context;
    }

    const char* message = PyUnicode_AsUTF8(rendered.get());
    if (message == nullptr) {
        PyErr_Clear();
        return context;
    }
    return context + ": " + message;
}

void requirePythonSuccess(bool success, const std::string& context)
{
    if (!success) {
        throw std::runtime_error(consumePythonError(context));
    }
}

PyRef makeFloatList(const std::vector<float>& values)
{
    PyRef list(PyList_New(static_cast<Py_ssize_t>(values.size())));
    requirePythonSuccess(list.get() != nullptr, "cannot allocate Python vector");

    for (std::size_t index = 0; index < values.size(); ++index) {
        PyObject* value = PyFloat_FromDouble(values[index]);
        requirePythonSuccess(value != nullptr, "cannot create Python float");
        PyList_SET_ITEM(list.get(), static_cast<Py_ssize_t>(index), value);
    }
    return list;
}

PyRef makeMatrixList(const std::vector<float>& values,
                     std::uint32_t rows,
                     std::uint32_t columns)
{
    PyRef matrix(PyList_New(rows));
    requirePythonSuccess(matrix.get() != nullptr, "cannot allocate Python matrix");

    for (std::uint32_t row = 0; row < rows; ++row) {
        PyObject* rowList = PyList_New(columns);
        requirePythonSuccess(rowList != nullptr, "cannot allocate Python matrix row");
        PyList_SET_ITEM(matrix.get(), row, rowList);

        for (std::uint32_t column = 0; column < columns; ++column) {
            const std::size_t index =
                static_cast<std::size_t>(row) * columns + column;
            PyObject* value = PyFloat_FromDouble(values[index]);
            requirePythonSuccess(value != nullptr, "cannot create Python float");
            PyList_SET_ITEM(rowList, column, value);
        }
    }
    return matrix;
}

} // namespace

struct CrossSimAnalogBackend::Implementation
{
    struct Array {
        PyObject* core = nullptr;
        PyObject* setMatrix = nullptr;
        PyObject* matvec = nullptr;
        std::vector<float> input;
        std::vector<float> output;
        bool matrixLoaded = false;
        bool inputLoaded = false;
        bool outputReady = false;
    };

    Implementation(std::uint32_t arrayCount,
                   std::uint32_t rows,
                   std::uint32_t columns,
                   const std::string& configurationPath) :
        rows(rows),
        columns(columns)
    {
        GilGuard gil;
        try {
            initialize(arrayCount, configurationPath);
        } catch (...) {
            releasePythonObjects();
            throw;
        }
    }

    ~Implementation()
    {
        if (Py_IsInitialized()) {
            GilGuard gil;
            releasePythonObjects();
        }
    }

    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;

    Array& array(std::uint32_t arrayId)
    {
        if (arrayId >= arrays.size()) {
            throw std::out_of_range(
                "CrossSim array ID " + std::to_string(arrayId) + " is invalid");
        }
        return arrays[arrayId];
    }

    const Array& array(std::uint32_t arrayId) const
    {
        if (arrayId >= arrays.size()) {
            throw std::out_of_range(
                "CrossSim array ID " + std::to_string(arrayId) + " is invalid");
        }
        return arrays[arrayId];
    }

    Array& ensureArray(std::uint32_t arrayId)
    {
        Array& state = array(arrayId);
        if (state.core != nullptr) {
            return state;
        }

        PyRef core(PyObject_Call(analogCoreClass, coreArguments, coreKeywords));
        requirePythonSuccess(
            core.get() != nullptr,
            "cannot construct CrossSim AnalogCore for array " +
                std::to_string(arrayId));

        PyRef setMatrix(PyObject_GetAttrString(core.get(), "set_matrix"));
        requirePythonSuccess(
            setMatrix.get() != nullptr,
            "CrossSim AnalogCore does not provide set_matrix");
        PyRef matvec(PyObject_GetAttrString(core.get(), "matvec"));
        requirePythonSuccess(
            matvec.get() != nullptr,
            "CrossSim AnalogCore does not provide matvec");

        state.input.resize(columns);
        state.output.resize(rows);
        state.core = core.release();
        state.setMatrix = setMatrix.release();
        state.matvec = matvec.release();
        return state;
    }

    PyRef asFloat32Array(PyObject* values) const
    {
        PyRef args(PyTuple_Pack(1, values));
        requirePythonSuccess(args.get() != nullptr,
                             "cannot create NumPy asarray arguments");
        PyRef keywords(PyDict_New());
        requirePythonSuccess(keywords.get() != nullptr,
                             "cannot create NumPy asarray keywords");
        requirePythonSuccess(
            PyDict_SetItemString(keywords.get(), "dtype", numpyFloat32) == 0,
            "cannot select NumPy float32");

        PyRef arrayObject(
            PyObject_Call(numpyAsArray, args.get(), keywords.get()));
        requirePythonSuccess(arrayObject.get() != nullptr,
                             "NumPy float32 conversion failed");
        return arrayObject;
    }

    std::vector<float> copyOutput(PyObject* result) const
    {
        PyRef toList(PyObject_CallMethod(result, "tolist", nullptr));
        requirePythonSuccess(toList.get() != nullptr,
                             "CrossSim output conversion failed");
        requirePythonSuccess(PyList_Check(toList.get()),
                             "CrossSim returned a non-vector output");

        const Py_ssize_t length = PyList_Size(toList.get());
        requirePythonSuccess(
            length == static_cast<Py_ssize_t>(rows),
            "CrossSim returned an output with the wrong length");

        std::vector<float> outputValues(rows);
        for (std::uint32_t index = 0; index < rows; ++index) {
            const double value =
                PyFloat_AsDouble(PyList_GET_ITEM(toList.get(), index));
            requirePythonSuccess(!PyErr_Occurred(),
                                 "CrossSim output contains a non-float value");
            outputValues[index] = static_cast<float>(value);
        }
        return outputValues;
    }

    void initialize(std::uint32_t arrayCount,
                    const std::string& configurationPath)
    {
        simulatorModule = PyImport_ImportModule("simulator");
        requirePythonSuccess(
            simulatorModule != nullptr,
            "cannot import CrossSim module 'simulator'; install CrossSim for "
            "SST's Python and include it in PYTHONPATH");

        numpyModule = PyImport_ImportModule("numpy");
        requirePythonSuccess(numpyModule != nullptr, "cannot import NumPy");

        numpyAsArray = PyObject_GetAttrString(numpyModule, "asarray");
        requirePythonSuccess(numpyAsArray != nullptr,
                             "NumPy does not provide asarray");
        numpyFloat32 = PyObject_GetAttrString(numpyModule, "float32");
        requirePythonSuccess(numpyFloat32 != nullptr,
                             "NumPy does not provide float32");

        PyRef parametersClass(
            PyObject_GetAttrString(simulatorModule, "CrossSimParameters"));
        requirePythonSuccess(parametersClass.get() != nullptr,
                             "CrossSim does not provide CrossSimParameters");

        if (configurationPath.empty()) {
            parameters = PyObject_CallNoArgs(parametersClass.get());
        } else {
            PyRef fromJson(
                PyObject_GetAttrString(parametersClass.get(), "from_json"));
            requirePythonSuccess(fromJson.get() != nullptr,
                                 "CrossSimParameters does not provide from_json");
            PyRef path(PyUnicode_FromString(configurationPath.c_str()));
            requirePythonSuccess(path.get() != nullptr,
                                 "cannot encode CrossSim configuration path");
            parameters = PyObject_CallOneArg(fromJson.get(), path.get());
        }
        requirePythonSuccess(parameters != nullptr,
                             "cannot construct CrossSim parameters");

        PyRef analogCoreClass(
            PyObject_GetAttrString(simulatorModule, "AnalogCore"));
        requirePythonSuccess(analogCoreClass.get() != nullptr,
                             "CrossSim does not provide AnalogCore");

        const std::vector<float> zeroValues(
            static_cast<std::size_t>(rows) * columns, 0.0F);
        PyRef zeroList(makeMatrixList(zeroValues, rows, columns));
        PyRef zeroMatrix(asFloat32Array(zeroList.get()));
        PyRef coreArguments(
            PyTuple_Pack(2, zeroMatrix.get(), parameters));
        requirePythonSuccess(
            coreArguments.get() != nullptr,
            "cannot create CrossSim AnalogCore arguments");
        PyRef coreKeywords(PyDict_New());
        requirePythonSuccess(
            coreKeywords.get() != nullptr,
            "cannot create CrossSim AnalogCore keywords");
        requirePythonSuccess(
            PyDict_SetItemString(
                coreKeywords.get(), "empty_matrix", Py_True) == 0,
            "cannot defer CrossSim matrix programming");

        this->analogCoreClass = analogCoreClass.release();
        this->coreArguments = coreArguments.release();
        this->coreKeywords = coreKeywords.release();
        arrays.resize(arrayCount);
    }

    void releasePythonObjects() noexcept
    {
        for (Array& state : arrays) {
            Py_XDECREF(state.matvec);
            Py_XDECREF(state.setMatrix);
            Py_XDECREF(state.core);
            state.matvec = nullptr;
            state.setMatrix = nullptr;
            state.core = nullptr;
        }
        arrays.clear();

        Py_XDECREF(coreKeywords);
        Py_XDECREF(coreArguments);
        Py_XDECREF(analogCoreClass);
        Py_XDECREF(parameters);
        Py_XDECREF(numpyFloat32);
        Py_XDECREF(numpyAsArray);
        Py_XDECREF(numpyModule);
        Py_XDECREF(simulatorModule);
        parameters = nullptr;
        numpyFloat32 = nullptr;
        numpyAsArray = nullptr;
        numpyModule = nullptr;
        simulatorModule = nullptr;
        coreKeywords = nullptr;
        coreArguments = nullptr;
        analogCoreClass = nullptr;
    }

    std::uint32_t rows;
    std::uint32_t columns;
    PyObject* simulatorModule = nullptr;
    PyObject* numpyModule = nullptr;
    PyObject* numpyAsArray = nullptr;
    PyObject* numpyFloat32 = nullptr;
    PyObject* analogCoreClass = nullptr;
    PyObject* coreArguments = nullptr;
    PyObject* coreKeywords = nullptr;
    PyObject* parameters = nullptr;
    std::vector<Array> arrays;
};

CrossSimAnalogBackend::CrossSimAnalogBackend(
    std::uint32_t arrayCount,
    std::uint32_t arrayRows,
    std::uint32_t arrayColumns,
    std::string configurationPath) :
    arrayRows_(arrayRows),
    arrayColumns_(arrayColumns)
{
    if (arrayCount == 0) {
        throw std::invalid_argument(
            "a CrossSim backend requires at least one array");
    }
    if (arrayRows_ == 0 || arrayColumns_ == 0) {
        throw std::invalid_argument(
            "CrossSim array dimensions must be nonzero");
    }

    implementation_ = std::make_unique<Implementation>(
        arrayCount,
        arrayRows_,
        arrayColumns_,
        configurationPath);
}

CrossSimAnalogBackend::~CrossSimAnalogBackend() = default;

std::size_t CrossSimAnalogBackend::arrayCount() const noexcept
{
    return implementation_->arrays.size();
}

void CrossSimAnalogBackend::setMatrix(
    std::uint32_t arrayId,
    const std::vector<float>& matrix)
{
    const std::size_t expected =
        static_cast<std::size_t>(arrayRows_) * arrayColumns_;
    if (matrix.size() != expected) {
        throw std::invalid_argument(
            "matrix size does not match CrossSim array shape");
    }

    GilGuard gil;
    Implementation::Array& target = implementation_->ensureArray(arrayId);
    PyRef matrixList(makeMatrixList(matrix, arrayRows_, arrayColumns_));
    PyRef matrixArray(implementation_->asFloat32Array(matrixList.get()));
    PyRef result(PyObject_CallOneArg(target.setMatrix, matrixArray.get()));
    requirePythonSuccess(result.get() != nullptr,
                         "CrossSim set_matrix failed");

    target.matrixLoaded = true;
    target.outputReady = false;
}

void CrossSimAnalogBackend::loadVector(
    std::uint32_t arrayId,
    const std::vector<float>& input)
{
    if (input.size() != arrayColumns_) {
        throw std::invalid_argument(
            "input size does not match CrossSim array shape");
    }

    Implementation::Array& target = implementation_->array(arrayId);
    target.input = input;
    target.inputLoaded = true;
    target.outputReady = false;
}

void CrossSimAnalogBackend::compute(std::uint32_t arrayId)
{
    Implementation::Array& target = implementation_->array(arrayId);
    if (!target.matrixLoaded) {
        throw std::logic_error(
            "cannot run CrossSim before loading a matrix");
    }
    if (!target.inputLoaded) {
        throw std::logic_error(
            "cannot run CrossSim before loading an input vector");
    }

    GilGuard gil;
    PyRef inputList(makeFloatList(target.input));
    PyRef inputArray(implementation_->asFloat32Array(inputList.get()));
    PyRef result(PyObject_CallOneArg(target.matvec, inputArray.get()));
    requirePythonSuccess(result.get() != nullptr,
                         "CrossSim matvec failed");

    target.output = implementation_->copyOutput(result.get());
    target.outputReady = true;
}

std::vector<float>
CrossSimAnalogBackend::output(std::uint32_t arrayId) const
{
    const Implementation::Array& source = implementation_->array(arrayId);
    if (!source.outputReady) {
        throw std::logic_error("CrossSim array output is not ready");
    }
    return source.output;
}

void CrossSimAnalogBackend::moveOutput(
    std::uint32_t sourceArrayId,
    std::uint32_t destinationArrayId)
{
    const Implementation::Array& source =
        implementation_->array(sourceArrayId);
    Implementation::Array& destination =
        implementation_->array(destinationArrayId);

    if (!source.outputReady) {
        throw std::logic_error(
            "source CrossSim array output is not ready");
    }
    if (source.output.size() != destination.input.size()) {
        throw std::invalid_argument(
            "source CrossSim output does not fit destination input");
    }

    destination.input = source.output;
    destination.inputLoaded = true;
    destination.outputReady = false;
}

} // namespace Mittens
} // namespace SST
