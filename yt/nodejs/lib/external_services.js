var Q = require("q");
var url = require("url");
var uuid = require("node-uuid");
var querystring = require("querystring");

var YtError = require("./error").that;
var YtRegistry = require("./registry").that;
var YtHttpRequest = require("./http_request").that;
var utils = require("./utils");

////////////////////////////////////////////////////////////////////////////////

exports.blackboxValidateToken = function(logger, party, token)
{
    "use strict";

    var config = YtRegistry.get("config", "services", "blackbox");
    var marker = uuid.v4();

    return (function inner(retry)
    {
        var tagged_logger = new utils.TaggedLogger(
            logger,
            { retry : retry, blackbox_marker : marker });

        if (retry >= config.retries) {
            var error = new YtError("Too many failed Blackbox requests");
            tagged_logger.error(error.message, { retries : config.retries });
            return Q.reject(error);
        }

        tagged_logger.debug("Querying Blackbox");

        return new YtHttpRequest(
            config.host,
            config.port)
        .withPath(url.format({
            pathname : "/blackbox",
            query : {
                method : "oauth",
                format : "json",
                userip : party,
                oauth_token : token
            }
        }))
        .withHeader("X-YT-Marker", marker)
        .setNoDelay(config.nodelay)
        .setTimeout(config.timeout)
        .asJson(true)
        .fire()
        .then(function(data) {
            if (typeof(data.exception) !== "undefined") {
                var error = new YtError(
                    "Blackbox returned an exception: " + data.exception);
                error.attributes.raw_data = data;
                tagged_logger.info(error.message, { data: data });
                return Q.reject(error);
            } else {
                tagged_logger.info(
                    "Successfully queried Blackbox",
                    { data: data });
                return data;
            }
        })
        .fail(function(err) {
            var error = YtError.ensureWrapped(err);
            tagged_logger.info("Retrying to query Blackbox", {
                // XXX(sandello): Embed.
                error: error.toJson()
            });
            return Q
            .delay(config.timeout * retry)
            .then(inner.bind(undefined, retry + 1));
        });
    })(0);
};

exports.oAuthObtainToken = function(logger, client_id, client_secret, code)
{
    "use strict";

    var config = YtRegistry.get("config", "services", "oauth");
    var marker = uuid.v4();

    return (function inner(retry) {
        var tagged_logger = new utils.TaggedLogger(
            logger,
            { retry : retry, oauth_marker : marker });

        if (retry >= config.retries) {
            var error = new YtError("Too many failed OAuth requests");
            tagged_logger.error(error.message, { retries : config.retries });
            return Q.reject(error);
        }

        tagged_logger.debug("Querying OAuth");

        return new YtHttpRequest(
            config.host,
            config.port)
        .withVerb("POST")
        .withPath("/token")
        .withBody(querystring.stringify({
            code : code,
            grant_type : "authorization_code",
            client_id : client_id,
            client_secret : client_secret
        }), "application/www-form-urlencoded")
        .withHeader("X-YT-Marker", marker)
        .setNoDelay(config.nodelay)
        .setTimeout(config.timeout)
        .asJson(true)
        .fire()
        .then(function(data) {
            if (typeof(data.error) !== "undefined") {
                var error = new YtError(
                    "OAuth returned an error: " + data.error);
                error.attributes.raw_data = data;
                tagged_logger.info(error.message, { data: data });
                return Q.reject(error);
            } else {
                tagged_logger.info(
                    "Successfully queried OAuth",
                    { data: data });
                return data;
            }
        })
        .fail(function(err) {
            var error = YtError.ensureWrapped(err);
            tagged_logger.info("Retrying to query OAuth", {
                // XXX(sandello): Embed.
                error: error.toJson()
            });
            return Q
            .delay(config.timeout * retry)
            .then(inner.bind(undefined, retry + 1));
        });
    })(0);
};

exports.oAuthBuildUrlToRedirect = function(client_id, state)
{
    "use strict";

    var config = YtRegistry.get("config", "services", "oauth");

    return url.format({
        protocol : "http",
        host : config.host,
        port : config.port,
        pathname : "/authorize",
        query : {
            response_type : "code",
            display : "popup",
            client_id : client_id,
            state : JSON.stringify(state)
        }
    });
};
