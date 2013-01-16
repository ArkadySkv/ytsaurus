var Q = require("q");

var YtError = require("./error").that;
var YtReadableStream = require("./readable_stream").that;
var YtWritableStream = require("./writable_stream").that;

var binding = require("./ytnode");

////////////////////////////////////////////////////////////////////////////////

var __DBG;

if (process.env.NODE_DEBUG && /YT(ALL|NODE)/.test(process.env.NODE_DEBUG)) {
    __DBG = function(x) { "use strict"; console.error("YT Driver:", x); };
    __DBG.UUID = require("node-uuid");
} else {
    __DBG = function(){};
}

////////////////////////////////////////////////////////////////////////////////

function promisinglyPipe(source, destination)
{
    "use strict";

    var deferred = Q.defer();

    var uuid = null;
    var debug = function(){};

    if (__DBG.UUID) {
        uuid = __DBG.UUID.v4();
        debug = function(x) { __DBG("Pipe " + uuid + " -> " + x); };
        debug("New");
    }

    function on_data(chunk) {
        if (destination.writable && destination.write(chunk) === false) {
            source.pause();
        }
    }

    source.on("data", on_data);

    function on_drain() {
        debug("on_drain");
        if (source.readable) {
            source.resume();
        }
    }

    destination.on("drain", on_drain);

    function on_end() {
        debug("Piping has ended");
        deferred.resolve();
    }
    function on_source_close() {
        debug("Source stream has been closed");
        deferred.reject(new YtError("Source stream in the pipe has been closed."));
    }
    function on_destination_close() {
        debug("Destination stream has been closed");
        deferred.reject(new YtError("Destination stream in the pipe has been closed."));
    }
    function on_error(err) {
        debug("An error occured");
        cleanup();
        deferred.reject(err);
    }

    source.on("end", on_end);
    source.on("close", on_source_close);
    source.on("error", on_error);

    destination.on("close", on_destination_close);
    destination.on("error", on_error);

    function cleanup() {
        debug("Cleaning up");

        source.removeListener("data", on_data);
        destination.removeListener("drain", on_drain);

        source.removeListener("end", on_end);
        source.removeListener("close", on_source_close);
        source.removeListener("error", on_error);

        destination.removeListener("close", on_destination_close);
        destination.removeListener("error", on_error);

        source.removeListener("end", cleanup);
        source.removeListener("close", cleanup);

        destination.removeListener("end", cleanup);
        destination.removeListener("close", cleanup);
    }

    source.on("end", cleanup);
    source.on("close", cleanup);

    destination.on("end", cleanup);
    destination.on("close", cleanup);

    destination.emit("pipe", source);

    return deferred.promise;
}

////////////////////////////////////////////////////////////////////////////////

function YtDriver(echo, config) {
    "use strict";

    if (__DBG.UUID) {
        this.__DBG  = function(x) { __DBG(this.__UUID + " -> " + x); };
        this.__UUID = __DBG.UUID.v4();
    } else {
        this.__DBG  = function(){};
    }

    this.__DBG("New");

    this.low_watermark = config.low_watermark;
    this.high_watermark = config.high_watermark;

    this.__DBG("low_watermark = " + this.low_watermark);
    this.__DBG("high_watermark = " + this.high_watermark);

    this._binding = new binding.TDriverWrap(echo, config.proxy);
}

YtDriver.prototype.execute = function(name,
    input_stream, input_compression, input_format,
    output_stream, output_compression, output_format,
    parameters
) {
    "use strict";
    this.__DBG("execute");

    var wrapped_input_stream = new YtWritableStream(this.low_watermark, this.high_watermark);
    var wrapped_output_stream = new YtReadableStream(this.low_watermark, this.high_watermark);

    this.__DBG("execute <<(" + wrapped_input_stream.__UUID + ") >>(" + wrapped_output_stream.__UUID + ")");

    var deferred = Q.defer();
    var self = this;

    var input_pipe_promise = Q.when(
        promisinglyPipe(input_stream, wrapped_input_stream),
        function() {
            self.__DBG("execute -> input_pipe_promise has been resolved");
            wrapped_input_stream.end();
        },
        function(err) {
            self.__DBG("execute -> input_pipe_promise has been rejected");
            input_stream.destroy();
            wrapped_input_stream.destroy();
            deferred.reject(new YtError("Input pipe has been cancelled", err));
        });

    var output_pipe_promise = Q.when(
        promisinglyPipe(wrapped_output_stream, output_stream),
        function() {
            // Do not close |output_stream| here since we have to write out trailers.
            self.__DBG("execute -> output_pipe_promise has been resolved");
        },
        function(err) {
            self.__DBG("execute -> output_pipe_promise has been rejected");
            output_stream.destroy();
            wrapped_output_stream.destroy();
            deferred.reject(new YtError("Output pipe has been cancelled", err));
        });

    this._binding.Execute(name,
        wrapped_input_stream._binding, input_compression, input_format,
        wrapped_output_stream._binding, output_compression, output_format,
        parameters, function(result)
    {
        self.__DBG("execute -> (on-execute callback)");
        // XXX(sandello): Can we move |_endSoon| to C++?
        wrapped_output_stream._endSoon();

        if (result.code === 0) {
            self.__DBG("execute -> execute_promise has been resolved");
            deferred.resolve(Array.prototype.slice.call(arguments));
        } else {
            wrapped_input_stream.destroy();
            wrapped_output_stream.destroy();
            self.__DBG("execute -> execute_promise has been rejected");
            deferred.reject(result);
        }
    });

    return Q
        .all([ deferred.promise, input_pipe_promise, output_pipe_promise ])
        .spread(function(result, ir, or) { return result; });
};

YtDriver.prototype.find_command_descriptor = function(command_name) {
    "use strict";
    this.__DBG("find_command_descriptor");
    return this._binding.FindCommandDescriptor(command_name);
};

YtDriver.prototype.get_command_descriptors = function() {
    "use strict";
    this.__DBG("get_command_descriptors");
    return this._binding.GetCommandDescriptors();
};

////////////////////////////////////////////////////////////////////////////////

exports.that = YtDriver;
