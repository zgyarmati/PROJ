/******************************************************************************
 *
 * Project:  PROJ
 * Purpose:  C API wraper of C++ API
 * Author:   Even Rouault <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2018, Even Rouault <even dot rouault at spatialys dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#ifndef FROM_PROJ_CPP
#define FROM_PROJ_CPP
#endif

#include <cassert>
#include <cstdarg>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "proj/common.hpp"
#include "proj/coordinateoperation.hpp"
#include "proj/coordinatesystem.hpp"
#include "proj/crs.hpp"
#include "proj/datum.hpp"
#include "proj/io.hpp"
#include "proj/metadata.hpp"
#include "proj/util.hpp"

#include "proj/internal/internal.hpp"

// PROJ include order is sensitive
// clang-format off
#include "proj_internal.h"
#include "proj.h"
#include "proj_experimental.h"
#include "projects.h"
// clang-format on
#include "proj_constants.h"

using namespace NS_PROJ::common;
using namespace NS_PROJ::crs;
using namespace NS_PROJ::cs;
using namespace NS_PROJ::datum;
using namespace NS_PROJ::io;
using namespace NS_PROJ::internal;
using namespace NS_PROJ::metadata;
using namespace NS_PROJ::operation;
using namespace NS_PROJ::util;
using namespace NS_PROJ;

// ---------------------------------------------------------------------------

static void PROJ_NO_INLINE proj_log_error(PJ_CONTEXT *ctx, const char *function,
                                          const char *text) {
    std::string msg(function);
    msg += ": ";
    msg += text;
    ctx->logger(ctx->app_data, PJ_LOG_ERROR, msg.c_str());
}

// ---------------------------------------------------------------------------

static void PROJ_NO_INLINE proj_log_debug(PJ_CONTEXT *ctx, const char *function,
                                          const char *text) {
    std::string msg(function);
    msg += ": ";
    msg += text;
    ctx->logger(ctx->app_data, PJ_LOG_DEBUG, msg.c_str());
}

// ---------------------------------------------------------------------------

/** \brief Opaque object representing a Ellipsoid, Datum, CRS or Coordinate
 * Operation. Should be used by at most one thread at a time. */
struct PJ_OBJ {
    //! @cond Doxygen_Suppress
    IdentifiedObjectNNPtr obj;

    // cached results
    mutable std::string lastWKT{};
    mutable std::string lastPROJString{};
    mutable bool gridsNeededAsked = false;
    mutable std::vector<GridDescription> gridsNeeded{};

    explicit PJ_OBJ(const IdentifiedObjectNNPtr &objIn) : obj(objIn) {}
    static PJ_OBJ *create(const IdentifiedObjectNNPtr &objIn);

    PJ_OBJ(const PJ_OBJ &) = delete;
    PJ_OBJ &operator=(const PJ_OBJ &) = delete;
    //! @endcond
};

//! @cond Doxygen_Suppress
PJ_OBJ *PJ_OBJ::create(const IdentifiedObjectNNPtr &objIn) {
    return new PJ_OBJ(objIn);
}
//! @endcond

// ---------------------------------------------------------------------------

/** \brief Opaque object representing a set of operation results. */
struct PJ_OBJ_LIST {
    //! @cond Doxygen_Suppress
    std::vector<IdentifiedObjectNNPtr> objects;

    explicit PJ_OBJ_LIST(std::vector<IdentifiedObjectNNPtr> &&objectsIn)
        : objects(std::move(objectsIn)) {}

    PJ_OBJ_LIST(const PJ_OBJ_LIST &) = delete;
    PJ_OBJ_LIST &operator=(const PJ_OBJ_LIST &) = delete;
    //! @endcond
};

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress

/** Auxiliary structure to PJ_CONTEXT storing C++ context stuff. */
struct projCppContext {
    DatabaseContextNNPtr databaseContext;
    std::string lastUOMName_{};

    explicit projCppContext(PJ_CONTEXT *ctx, const char *dbPath = nullptr,
                            const char *const *auxDbPaths = nullptr)
        : databaseContext(DatabaseContext::create(
              dbPath ? dbPath : std::string(), toVector(auxDbPaths))) {
        databaseContext->attachPJContext(ctx);
    }

    static std::vector<std::string> toVector(const char *const *auxDbPaths) {
        std::vector<std::string> res;
        for (auto iter = auxDbPaths; iter && *iter; ++iter) {
            res.emplace_back(std::string(*iter));
        }
        return res;
    }
};

// ---------------------------------------------------------------------------

void proj_context_delete_cpp_context(struct projCppContext *cppContext) {
    delete cppContext;
}

//! @endcond

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress

#define SANITIZE_CTX(ctx)                                                      \
    do {                                                                       \
        if (ctx == nullptr) {                                                  \
            ctx = pj_get_default_ctx();                                        \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------

static PROJ_NO_INLINE const DatabaseContextNNPtr &
getDBcontext(PJ_CONTEXT *ctx) {
    if (ctx->cpp_context == nullptr) {
        ctx->cpp_context = new projCppContext(ctx);
    }
    return ctx->cpp_context->databaseContext;
}

// ---------------------------------------------------------------------------

static PROJ_NO_INLINE DatabaseContextPtr
getDBcontextNoException(PJ_CONTEXT *ctx, const char *function) {
    try {
        return getDBcontext(ctx).as_nullable();
    } catch (const std::exception &e) {
        proj_log_debug(ctx, function, e.what());
        return nullptr;
    }
}

//! @endcond

// ---------------------------------------------------------------------------

/** \brief Explicitly point to the main PROJ CRS and coordinate operation
 * definition database ("proj.db"), and potentially auxiliary databases with
 * same structure.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param dbPath Path to main database, or NULL for default.
 * @param auxDbPaths NULL-terminated list of auxiliary database filenames, or
 * NULL.
 * @param options should be set to NULL for now
 * @return TRUE in case of success
 */
int proj_context_set_database_path(PJ_CONTEXT *ctx, const char *dbPath,
                                   const char *const *auxDbPaths,
                                   const char *const *options) {
    SANITIZE_CTX(ctx);
    (void)options;
    delete ctx->cpp_context;
    ctx->cpp_context = nullptr;
    try {
        ctx->cpp_context = new projCppContext(ctx, dbPath, auxDbPaths);
        return true;
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------

/** \brief Returns the path to the database.
 *
 * The returned pointer remains valid while ctx is valid, and until
 * proj_context_set_database_path() is called.
 *
 * @param ctx PROJ context, or NULL for default context
 * @return path, or nullptr
 */
const char *proj_context_get_database_path(PJ_CONTEXT *ctx) {
    SANITIZE_CTX(ctx);
    try {
        return getDBcontext(ctx)->getPath().c_str();
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return a metadata from the database.
 *
 * The returned pointer remains valid while ctx is valid, and until
 * proj_context_get_database_metadata() is called.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param key Metadata key. Must not be NULL
 * @return value, or nullptr
 */
const char *proj_context_get_database_metadata(PJ_CONTEXT *ctx,
                                               const char *key) {
    SANITIZE_CTX(ctx);
    try {
        return getDBcontext(ctx)->getMetadata(key);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Guess the "dialect" of the WKT string.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param wkt String (must not be NULL)
 */
PJ_GUESSED_WKT_DIALECT proj_context_guess_wkt_dialect(PJ_CONTEXT *ctx,
                                                      const char *wkt) {
    (void)ctx;
    assert(wkt);
    switch (WKTParser().guessDialect(wkt)) {
    case WKTParser::WKTGuessedDialect::WKT2_2018:
        return PJ_GUESSED_WKT2_2018;
    case WKTParser::WKTGuessedDialect::WKT2_2015:
        return PJ_GUESSED_WKT2_2015;
    case WKTParser::WKTGuessedDialect::WKT1_GDAL:
        return PJ_GUESSED_WKT1_GDAL;
    case WKTParser::WKTGuessedDialect::WKT1_ESRI:
        return PJ_GUESSED_WKT1_ESRI;
    case WKTParser::WKTGuessedDialect::NOT_WKT:
        break;
    }
    return PJ_GUESSED_NOT_WKT;
}

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress
static const char *getOptionValue(const char *option,
                                  const char *keyWithEqual) noexcept {
    if (ci_starts_with(option, keyWithEqual)) {
        return option + strlen(keyWithEqual);
    }
    return nullptr;
}
//! @endcond

// ---------------------------------------------------------------------------

/** \brief "Clone" an object.
 *
 * Technically this just increases the reference counter on the object, since
 * PJ_OBJ objects are immutable.
 *
 * The returned object must be unreferenced with proj_obj_unref() after use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object to clone. Must not be NULL.
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL in
 * case of error.
 */
PJ_OBJ *proj_obj_clone(PJ_CONTEXT *ctx, const PJ_OBJ *obj) {
    SANITIZE_CTX(ctx);
    try {
        return PJ_OBJ::create(obj->obj);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate an object from a WKT string, PROJ string or object code
 * (like "EPSG:4326", "urn:ogc:def:crs:EPSG::4326",
 * "urn:ogc:def:coordinateOperation:EPSG::1671").
 *
 * This function calls osgeo::proj::io::createFromUserInput()
 *
 * The returned object must be unreferenced with proj_obj_unref() after use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param text String (must not be NULL)
 * @param options null-terminated list of options, or NULL. Currently
 * supported options are:
 * <ul>
 * <li>USE_PROJ4_INIT_RULES=YES/NO. Defaults to NO. When set to YES,
 * init=epsg:XXXX syntax will be allowed and will be interpreted according to
 * PROJ.4 and PROJ.5 rules, that is geodeticCRS will have longitude, latitude
 * order and will expect/output coordinates in radians. ProjectedCRS will have
 * easting, northing axis order (except the ones with Transverse Mercator South
 * Orientated projection). In that mode, the epsg:XXXX syntax will be also
 * interprated the same way.</li>
 * </ul>
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL in
 * case of error.
 */
PJ_OBJ *proj_obj_create_from_user_input(PJ_CONTEXT *ctx, const char *text,
                                        const char *const *options) {
    SANITIZE_CTX(ctx);
    assert(text);
    (void)options;
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    try {
        bool usePROJ4InitRules = false;
        for (auto iter = options; iter && iter[0]; ++iter) {
            const char *value;
            if ((value = getOptionValue(*iter, "USE_PROJ4_INIT_RULES="))) {
                usePROJ4InitRules = ci_equal(value, "YES");
            } else {
                std::string msg("Unknown option :");
                msg += *iter;
                proj_log_error(ctx, __FUNCTION__, msg.c_str());
                return nullptr;
            }
        }
        auto identifiedObject = nn_dynamic_pointer_cast<IdentifiedObject>(
            createFromUserInput(text, dbContext, usePROJ4InitRules));
        if (identifiedObject) {
            return PJ_OBJ::create(NN_NO_CHECK(identifiedObject));
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate an object from a WKT string.
 *
 * This function calls osgeo::proj::io::WKTParser::createFromWKT()
 *
 * The returned object must be unreferenced with proj_obj_unref() after use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param wkt WKT string (must not be NULL)
 * @param options should be set to NULL for now
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL in
 * case of error.
 */
PJ_OBJ *proj_obj_create_from_wkt(PJ_CONTEXT *ctx, const char *wkt,
                                 const char *const *options) {
    SANITIZE_CTX(ctx);
    assert(wkt);
    (void)options;
    try {
        WKTParser parser;
        auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
        if (dbContext) {
            parser.attachDatabaseContext(NN_NO_CHECK(dbContext));
        }
        auto identifiedObject = nn_dynamic_pointer_cast<IdentifiedObject>(
            parser.createFromWKT(wkt));
        if (identifiedObject) {
            return PJ_OBJ::create(NN_NO_CHECK(identifiedObject));
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate an object from a PROJ string.
 *
 * This function calls osgeo::proj::io::PROJStringParser::createFromPROJString()
 *
 * The returned object must be unreferenced with proj_obj_unref() after use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param proj_string PROJ string (must not be NULL)
 * @param options should be set to NULL for now
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL in
 * case of error.
 */
PJ_OBJ *proj_obj_create_from_proj_string(PJ_CONTEXT *ctx,
                                         const char *proj_string,
                                         const char *const *options) {
    SANITIZE_CTX(ctx);
    (void)options;
    assert(proj_string);
    try {
        PROJStringParser parser;
        auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
        if (dbContext) {
            parser.attachDatabaseContext(NN_NO_CHECK(dbContext));
        }
        auto identifiedObject = nn_dynamic_pointer_cast<IdentifiedObject>(
            parser.createFromPROJString(proj_string));
        if (identifiedObject) {
            return PJ_OBJ::create(NN_NO_CHECK(identifiedObject));
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate an object from a database lookup.
 *
 * The returned object must be unreferenced with proj_obj_unref() after use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx Context, or NULL for default context.
 * @param auth_name Authority name (must not be NULL)
 * @param code Object code (must not be NULL)
 * @param category Object category
 * @param usePROJAlternativeGridNames Whether PROJ alternative grid names
 * should be substituted to the official grid names. Only used on
 * transformations
 * @param options should be set to NULL for now
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL in
 * case of error.
 */
PJ_OBJ *proj_obj_create_from_database(PJ_CONTEXT *ctx, const char *auth_name,
                                      const char *code,
                                      PJ_OBJ_CATEGORY category,
                                      int usePROJAlternativeGridNames,
                                      const char *const *options) {
    assert(auth_name);
    assert(code);
    (void)options;
    SANITIZE_CTX(ctx);
    try {
        const std::string codeStr(code);
        auto factory = AuthorityFactory::create(getDBcontext(ctx), auth_name);
        IdentifiedObjectPtr obj;
        switch (category) {
        case PJ_OBJ_CATEGORY_ELLIPSOID:
            obj = factory->createEllipsoid(codeStr).as_nullable();
            break;
        case PJ_OBJ_CATEGORY_PRIME_MERIDIAN:
            obj = factory->createPrimeMeridian(codeStr).as_nullable();
            break;
        case PJ_OBJ_CATEGORY_DATUM:
            obj = factory->createDatum(codeStr).as_nullable();
            break;
        case PJ_OBJ_CATEGORY_CRS:
            obj =
                factory->createCoordinateReferenceSystem(codeStr).as_nullable();
            break;
        case PJ_OBJ_CATEGORY_COORDINATE_OPERATION:
            obj = factory
                      ->createCoordinateOperation(
                          codeStr, usePROJAlternativeGridNames != 0)
                      .as_nullable();
            break;
        }
        return PJ_OBJ::create(NN_NO_CHECK(obj));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress
static const char *get_unit_category(UnitOfMeasure::Type type) {
    const char *ret = nullptr;
    switch (type) {
    case UnitOfMeasure::Type::UNKNOWN:
        ret = "unknown";
        break;
    case UnitOfMeasure::Type::NONE:
        ret = "none";
        break;
    case UnitOfMeasure::Type::ANGULAR:
        ret = "angular";
        break;
    case UnitOfMeasure::Type::LINEAR:
        ret = "linear";
        break;
    case UnitOfMeasure::Type::SCALE:
        ret = "scale";
        break;
    case UnitOfMeasure::Type::TIME:
        ret = "time";
        break;
    case UnitOfMeasure::Type::PARAMETRIC:
        ret = "parametric";
        break;
    }
    return ret;
}
//! @endcond

// ---------------------------------------------------------------------------

/** \brief Get information for a unit of measure from a database lookup.
 *
 * @param ctx Context, or NULL for default context.
 * @param auth_name Authority name (must not be NULL)
 * @param code Unit of measure code (must not be NULL)
 * @param out_name Pointer to a string value to store the parameter name. or
 * NULL. This value remains valid until the next call to
 * proj_uom_get_info_from_database() or the context destruction.
 * @param out_conv_factor Pointer to a value to store the conversion
 * factor of the prime meridian longitude unit to radian. or NULL
 * @param out_category Pointer to a string value to store the parameter name. or
 * NULL. This value might be "unknown", "none", "linear", "angular", "scale",
 * "time" or "parametric";
 * @return TRUE in case of success
 */
int proj_uom_get_info_from_database(PJ_CONTEXT *ctx, const char *auth_name,
                                    const char *code, const char **out_name,
                                    double *out_conv_factor,
                                    const char **out_category) {
    assert(auth_name);
    assert(code);
    SANITIZE_CTX(ctx);
    try {
        auto factory = AuthorityFactory::create(getDBcontext(ctx), auth_name);
        auto obj = factory->createUnitOfMeasure(code);
        if (out_name) {
            ctx->cpp_context->lastUOMName_ = obj->name();
            *out_name = ctx->cpp_context->lastUOMName_.c_str();
        }
        if (out_conv_factor) {
            *out_conv_factor = obj->conversionToSI();
        }
        if (out_category) {
            *out_category = get_unit_category(obj->type());
        }
        return true;
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return false;
}

// ---------------------------------------------------------------------------

/** \brief Return GeodeticCRS that use the specified datum.
 *
 * @param ctx Context, or NULL for default context.
 * @param crs_auth_name CRS authority name, or NULL.
 * @param datum_auth_name Datum authority name (must not be NULL)
 * @param datum_code Datum code (must not be NULL)
 * @param crs_type "geographic 2D", "geographic 3D", "geocentric" or NULL
 * @return a result set that must be unreferenced with
 * proj_obj_list_unref(), or NULL in case of error.
 */
PJ_OBJ_LIST *proj_obj_query_geodetic_crs_from_datum(PJ_CONTEXT *ctx,
                                                    const char *crs_auth_name,
                                                    const char *datum_auth_name,
                                                    const char *datum_code,
                                                    const char *crs_type) {
    assert(datum_auth_name);
    assert(datum_code);
    SANITIZE_CTX(ctx);
    try {
        auto factory = AuthorityFactory::create(
            getDBcontext(ctx), crs_auth_name ? crs_auth_name : "");
        auto res = factory->createGeodeticCRSFromDatum(
            datum_auth_name, datum_code, crs_type ? crs_type : "");
        std::vector<IdentifiedObjectNNPtr> objects;
        for (const auto &obj : res) {
            objects.push_back(obj);
        }
        return new PJ_OBJ_LIST(std::move(objects));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Drops a reference on an object.
 *
 * This method should be called one and exactly one for each function
 * returning a PJ_OBJ*
 *
 * @param obj Object, or NULL.
 */
void proj_obj_unref(PJ_OBJ *obj) { delete obj; }

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress
static AuthorityFactory::ObjectType
convertPJObjectTypeToObjectType(PJ_OBJ_TYPE type, bool &valid) {
    valid = true;
    AuthorityFactory::ObjectType cppType = AuthorityFactory::ObjectType::CRS;
    switch (type) {
    case PJ_OBJ_TYPE_ELLIPSOID:
        cppType = AuthorityFactory::ObjectType::ELLIPSOID;
        break;

    case PJ_OBJ_TYPE_PRIME_MERIDIAN:
        cppType = AuthorityFactory::ObjectType::PRIME_MERIDIAN;
        break;

    case PJ_OBJ_TYPE_GEODETIC_REFERENCE_FRAME:
    case PJ_OBJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME:
        cppType = AuthorityFactory::ObjectType::GEODETIC_REFERENCE_FRAME;
        break;

    case PJ_OBJ_TYPE_VERTICAL_REFERENCE_FRAME:
    case PJ_OBJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME:
        cppType = AuthorityFactory::ObjectType::VERTICAL_REFERENCE_FRAME;
        break;

    case PJ_OBJ_TYPE_DATUM_ENSEMBLE:
        cppType = AuthorityFactory::ObjectType::DATUM;
        break;

    case PJ_OBJ_TYPE_CRS:
        cppType = AuthorityFactory::ObjectType::CRS;
        break;

    case PJ_OBJ_TYPE_GEODETIC_CRS:
        cppType = AuthorityFactory::ObjectType::GEODETIC_CRS;
        break;

    case PJ_OBJ_TYPE_GEOCENTRIC_CRS:
        cppType = AuthorityFactory::ObjectType::GEOCENTRIC_CRS;
        break;

    case PJ_OBJ_TYPE_GEOGRAPHIC_CRS:
        cppType = AuthorityFactory::ObjectType::GEOGRAPHIC_CRS;
        break;

    case PJ_OBJ_TYPE_GEOGRAPHIC_2D_CRS:
        cppType = AuthorityFactory::ObjectType::GEOGRAPHIC_2D_CRS;
        break;

    case PJ_OBJ_TYPE_GEOGRAPHIC_3D_CRS:
        cppType = AuthorityFactory::ObjectType::GEOGRAPHIC_3D_CRS;
        break;

    case PJ_OBJ_TYPE_VERTICAL_CRS:
        cppType = AuthorityFactory::ObjectType::VERTICAL_CRS;
        break;

    case PJ_OBJ_TYPE_PROJECTED_CRS:
        cppType = AuthorityFactory::ObjectType::PROJECTED_CRS;
        break;

    case PJ_OBJ_TYPE_COMPOUND_CRS:
        cppType = AuthorityFactory::ObjectType::COMPOUND_CRS;
        break;

    case PJ_OBJ_TYPE_ENGINEERING_CRS:
        valid = false;
        break;

    case PJ_OBJ_TYPE_TEMPORAL_CRS:
        valid = false;
        break;

    case PJ_OBJ_TYPE_BOUND_CRS:
        valid = false;
        break;

    case PJ_OBJ_TYPE_OTHER_CRS:
        cppType = AuthorityFactory::ObjectType::CRS;
        break;

    case PJ_OBJ_TYPE_CONVERSION:
        cppType = AuthorityFactory::ObjectType::CONVERSION;
        break;

    case PJ_OBJ_TYPE_TRANSFORMATION:
        cppType = AuthorityFactory::ObjectType::TRANSFORMATION;
        break;

    case PJ_OBJ_TYPE_CONCATENATED_OPERATION:
        cppType = AuthorityFactory::ObjectType::CONCATENATED_OPERATION;
        break;

    case PJ_OBJ_TYPE_OTHER_COORDINATE_OPERATION:
        cppType = AuthorityFactory::ObjectType::COORDINATE_OPERATION;
        break;

    case PJ_OBJ_TYPE_UNKNOWN:
        valid = false;
        break;
    }
    return cppType;
}
//! @endcond

// ---------------------------------------------------------------------------

/** \brief Return a list of objects by their name.
 *
 * @param ctx Context, or NULL for default context.
 * @param auth_name Authority name, used to restrict the search.
 * Or NULL for all authorities.
 * @param searchedName Searched name. Must be at least 2 character long.
 * @param types List of object types into which to search. If
 * NULL, all object types will be searched.
 * @param typesCount Number of elements in types, or 0 if types is NULL
 * @param approximateMatch Whether approximate name identification is allowed.
 * @param limitResultCount Maximum number of results to return.
 * Or 0 for unlimited.
 * @param options should be set to NULL for now
 * @return a result set that must be unreferenced with
 * proj_obj_list_unref(), or NULL in case of error.
 */
PJ_OBJ_LIST *proj_obj_create_from_name(PJ_CONTEXT *ctx, const char *auth_name,
                                       const char *searchedName,
                                       const PJ_OBJ_TYPE *types,
                                       size_t typesCount, int approximateMatch,
                                       size_t limitResultCount,
                                       const char *const *options) {
    assert(searchedName);
    assert((types != nullptr && typesCount > 0) ||
           (types == nullptr && typesCount == 0));
    (void)options;
    SANITIZE_CTX(ctx);
    try {
        auto factory = AuthorityFactory::create(getDBcontext(ctx),
                                                auth_name ? auth_name : "");
        std::vector<AuthorityFactory::ObjectType> allowedTypes;
        for (size_t i = 0; i < typesCount; ++i) {
            bool valid = false;
            auto type = convertPJObjectTypeToObjectType(types[i], valid);
            if (valid) {
                allowedTypes.push_back(type);
            }
        }
        auto res = factory->createObjectsFromName(searchedName, allowedTypes,
                                                  approximateMatch != 0,
                                                  limitResultCount);
        std::vector<IdentifiedObjectNNPtr> objects;
        for (const auto &obj : res) {
            objects.push_back(obj);
        }
        return new PJ_OBJ_LIST(std::move(objects));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return the type of an object.
 *
 * @param obj Object (must not be NULL)
 * @return its type.
 */
PJ_OBJ_TYPE proj_obj_get_type(const PJ_OBJ *obj) {
    assert(obj);
    auto ptr = obj->obj.get();
    if (dynamic_cast<Ellipsoid *>(ptr)) {
        return PJ_OBJ_TYPE_ELLIPSOID;
    }

    if (dynamic_cast<PrimeMeridian *>(ptr)) {
        return PJ_OBJ_TYPE_PRIME_MERIDIAN;
    }

    if (dynamic_cast<DynamicGeodeticReferenceFrame *>(ptr)) {
        return PJ_OBJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME;
    }
    if (dynamic_cast<GeodeticReferenceFrame *>(ptr)) {
        return PJ_OBJ_TYPE_GEODETIC_REFERENCE_FRAME;
    }
    if (dynamic_cast<DynamicVerticalReferenceFrame *>(ptr)) {
        return PJ_OBJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME;
    }
    if (dynamic_cast<VerticalReferenceFrame *>(ptr)) {
        return PJ_OBJ_TYPE_VERTICAL_REFERENCE_FRAME;
    }
    if (dynamic_cast<DatumEnsemble *>(ptr)) {
        return PJ_OBJ_TYPE_DATUM_ENSEMBLE;
    }

    {
        auto crs = dynamic_cast<GeographicCRS *>(ptr);
        if (crs) {
            if (crs->coordinateSystem()->axisList().size() == 2) {
                return PJ_OBJ_TYPE_GEOGRAPHIC_2D_CRS;
            } else {
                return PJ_OBJ_TYPE_GEOGRAPHIC_3D_CRS;
            }
        }
    }

    {
        auto crs = dynamic_cast<GeodeticCRS *>(ptr);
        if (crs) {
            if (crs->isGeocentric()) {
                return PJ_OBJ_TYPE_GEOCENTRIC_CRS;
            } else {
                return PJ_OBJ_TYPE_GEODETIC_CRS;
            }
        }
    }

    if (dynamic_cast<VerticalCRS *>(ptr)) {
        return PJ_OBJ_TYPE_VERTICAL_CRS;
    }
    if (dynamic_cast<ProjectedCRS *>(ptr)) {
        return PJ_OBJ_TYPE_PROJECTED_CRS;
    }
    if (dynamic_cast<CompoundCRS *>(ptr)) {
        return PJ_OBJ_TYPE_COMPOUND_CRS;
    }
    if (dynamic_cast<TemporalCRS *>(ptr)) {
        return PJ_OBJ_TYPE_TEMPORAL_CRS;
    }
    if (dynamic_cast<EngineeringCRS *>(ptr)) {
        return PJ_OBJ_TYPE_ENGINEERING_CRS;
    }
    if (dynamic_cast<BoundCRS *>(ptr)) {
        return PJ_OBJ_TYPE_BOUND_CRS;
    }
    if (dynamic_cast<CRS *>(ptr)) {
        return PJ_OBJ_TYPE_OTHER_CRS;
    }

    if (dynamic_cast<Conversion *>(ptr)) {
        return PJ_OBJ_TYPE_CONVERSION;
    }
    if (dynamic_cast<Transformation *>(ptr)) {
        return PJ_OBJ_TYPE_TRANSFORMATION;
    }
    if (dynamic_cast<ConcatenatedOperation *>(ptr)) {
        return PJ_OBJ_TYPE_CONCATENATED_OPERATION;
    }
    if (dynamic_cast<CoordinateOperation *>(ptr)) {
        return PJ_OBJ_TYPE_OTHER_COORDINATE_OPERATION;
    }

    return PJ_OBJ_TYPE_UNKNOWN;
}

// ---------------------------------------------------------------------------

/** \brief Return whether an object is deprecated.
 *
 * @param obj Object (must not be NULL)
 * @return TRUE if it is deprecated, FALSE otherwise
 */
int proj_obj_is_deprecated(const PJ_OBJ *obj) {
    assert(obj);
    return obj->obj->isDeprecated();
}

// ---------------------------------------------------------------------------

/** \brief Return a list of non-deprecated objects related to the passed one
 *
 * @param ctx Context, or NULL for default context.
 * @param obj Object (of type CRS for now) for which non-deprecated objects
 * must be searched. Must not be NULL
 * @return a result set that must be unreferenced with
 * proj_obj_list_unref(), or NULL in case of error.
 */
PJ_OBJ_LIST *proj_obj_get_non_deprecated(PJ_CONTEXT *ctx, const PJ_OBJ *obj) {
    assert(obj);
    SANITIZE_CTX(ctx);
    auto crs = dynamic_cast<const CRS *>(obj->obj.get());
    if (!crs) {
        return nullptr;
    }
    try {
        std::vector<IdentifiedObjectNNPtr> objects;
        auto res = crs->getNonDeprecated(getDBcontext(ctx));
        for (const auto &resObj : res) {
            objects.push_back(resObj);
        }
        return new PJ_OBJ_LIST(std::move(objects));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return whether two objects are equivalent.
 *
 * @param obj Object (must not be NULL)
 * @param other Other object (must not be NULL)
 * @param criterion Comparison criterion
 * @return TRUE if they are equivalent
 */
int proj_obj_is_equivalent_to(const PJ_OBJ *obj, const PJ_OBJ *other,
                              PJ_COMPARISON_CRITERION criterion) {
    assert(obj);
    assert(other);

    // Make sure that the C and C++ enumerations match
    static_assert(static_cast<int>(PJ_COMP_STRICT) ==
                      static_cast<int>(IComparable::Criterion::STRICT),
                  "");
    static_assert(static_cast<int>(PJ_COMP_EQUIVALENT) ==
                      static_cast<int>(IComparable::Criterion::EQUIVALENT),
                  "");
    static_assert(
        static_cast<int>(PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS) ==
            static_cast<int>(
                IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS),
        "");

    // Make sure we enumerate all values. If adding a new value, as we
    // don't have a default clause, the compiler will warn.
    switch (criterion) {
    case PJ_COMP_STRICT:
    case PJ_COMP_EQUIVALENT:
    case PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS:
        break;
    }
    const IComparable::Criterion cppCriterion =
        static_cast<IComparable::Criterion>(criterion);
    return obj->obj->isEquivalentTo(other->obj.get(), cppCriterion);
}

// ---------------------------------------------------------------------------

/** \brief Return whether an object is a CRS
 *
 * @param obj Object (must not be NULL)
 */
int proj_obj_is_crs(const PJ_OBJ *obj) {
    assert(obj);
    return dynamic_cast<CRS *>(obj->obj.get()) != nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Get the name of an object.
 *
 * The lifetime of the returned string is the same as the input obj parameter.
 *
 * @param obj Object (must not be NULL)
 * @return a string, or NULL in case of error or missing name.
 */
const char *proj_obj_get_name(const PJ_OBJ *obj) {
    assert(obj);
    const auto &desc = obj->obj->name()->description();
    if (!desc.has_value()) {
        return nullptr;
    }
    // The object will still be alived after the function call.
    // cppcheck-suppress stlcstr
    return desc->c_str();
}

// ---------------------------------------------------------------------------

/** \brief Get the authority name / codespace of an identifier of an object.
 *
 * The lifetime of the returned string is the same as the input obj parameter.
 *
 * @param obj Object (must not be NULL)
 * @param index Index of the identifier. 0 = first identifier
 * @return a string, or NULL in case of error or missing name.
 */
const char *proj_obj_get_id_auth_name(const PJ_OBJ *obj, int index) {
    assert(obj);
    const auto &ids = obj->obj->identifiers();
    if (static_cast<size_t>(index) >= ids.size()) {
        return nullptr;
    }
    const auto &codeSpace = ids[index]->codeSpace();
    if (!codeSpace.has_value()) {
        return nullptr;
    }
    // The object will still be alived after the function call.
    // cppcheck-suppress stlcstr
    return codeSpace->c_str();
}

// ---------------------------------------------------------------------------

/** \brief Get the code of an identifier of an object.
 *
 * The lifetime of the returned string is the same as the input obj parameter.
 *
 * @param obj Object (must not be NULL)
 * @param index Index of the identifier. 0 = first identifier
 * @return a string, or NULL in case of error or missing name.
 */
const char *proj_obj_get_id_code(const PJ_OBJ *obj, int index) {
    assert(obj);
    const auto &ids = obj->obj->identifiers();
    if (static_cast<size_t>(index) >= ids.size()) {
        return nullptr;
    }
    return ids[index]->code().c_str();
}

// ---------------------------------------------------------------------------

/** \brief Get a WKT representation of an object.
 *
 * The returned string is valid while the input obj parameter is valid,
 * and until a next call to proj_obj_as_wkt() with the same input object.
 *
 * This function calls osgeo::proj::io::IWKTExportable::exportToWKT().
 *
 * This function may return NULL if the object is not compatible with an
 * export to the requested type.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object (must not be NULL)
 * @param type WKT version.
 * @param options null-terminated list of options, or NULL. Currently
 * supported options are:
 * <ul>
 * <li>MULTILINE=YES/NO. Defaults to YES, except for WKT1_ESRI</li>
 * <li>INDENTATION_WIDTH=number. Defauls to 4 (when multiline output is
 * on).</li>
 * <li>OUTPUT_AXIS=AUTO/YES/NO. In AUTO mode, axis will be output for WKT2
 * variants, for WKT1_GDAL for ProjectedCRS with easting/northing ordering
 * (otherwise stripped), but not for WKT1_ESRI. Setting to YES will output
 * them unconditionaly, and to NO will omit them unconditionaly.</li>
 * </ul>
 * @return a string, or NULL in case of error.
 */
const char *proj_obj_as_wkt(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                            PJ_WKT_TYPE type, const char *const *options) {
    SANITIZE_CTX(ctx);
    assert(obj);

    // Make sure that the C and C++ enumerations match
    static_assert(static_cast<int>(PJ_WKT2_2015) ==
                      static_cast<int>(WKTFormatter::Convention::WKT2_2015),
                  "");
    static_assert(
        static_cast<int>(PJ_WKT2_2015_SIMPLIFIED) ==
            static_cast<int>(WKTFormatter::Convention::WKT2_2015_SIMPLIFIED),
        "");
    static_assert(static_cast<int>(PJ_WKT2_2018) ==
                      static_cast<int>(WKTFormatter::Convention::WKT2_2018),
                  "");
    static_assert(
        static_cast<int>(PJ_WKT2_2018_SIMPLIFIED) ==
            static_cast<int>(WKTFormatter::Convention::WKT2_2018_SIMPLIFIED),
        "");
    static_assert(static_cast<int>(PJ_WKT1_GDAL) ==
                      static_cast<int>(WKTFormatter::Convention::WKT1_GDAL),
                  "");
    static_assert(static_cast<int>(PJ_WKT1_ESRI) ==
                      static_cast<int>(WKTFormatter::Convention::WKT1_ESRI),
                  "");
    // Make sure we enumerate all values. If adding a new value, as we
    // don't have a default clause, the compiler will warn.
    switch (type) {
    case PJ_WKT2_2015:
    case PJ_WKT2_2015_SIMPLIFIED:
    case PJ_WKT2_2018:
    case PJ_WKT2_2018_SIMPLIFIED:
    case PJ_WKT1_GDAL:
    case PJ_WKT1_ESRI:
        break;
    }
    const WKTFormatter::Convention convention =
        static_cast<WKTFormatter::Convention>(type);
    try {
        auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
        auto formatter = WKTFormatter::create(convention, dbContext);
        for (auto iter = options; iter && iter[0]; ++iter) {
            const char *value;
            if ((value = getOptionValue(*iter, "MULTILINE="))) {
                formatter->setMultiLine(ci_equal(value, "YES"));
            } else if ((value = getOptionValue(*iter, "INDENTATION_WIDTH="))) {
                formatter->setIndentationWidth(std::atoi(value));
            } else if ((value = getOptionValue(*iter, "OUTPUT_AXIS="))) {
                if (!ci_equal(value, "AUTO")) {
                    formatter->setOutputAxis(
                        ci_equal(value, "YES")
                            ? WKTFormatter::OutputAxisRule::YES
                            : WKTFormatter::OutputAxisRule::NO);
                }
            } else {
                std::string msg("Unknown option :");
                msg += *iter;
                proj_log_error(ctx, __FUNCTION__, msg.c_str());
                return nullptr;
            }
        }
        obj->lastWKT = obj->obj->exportToWKT(formatter.get());
        return obj->lastWKT.c_str();
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Get a PROJ string representation of an object.
 *
 * The returned string is valid while the input obj parameter is valid,
 * and until a next call to proj_obj_as_proj_string() with the same input
 * object.
 *
 * This function calls
 * osgeo::proj::io::IPROJStringExportable::exportToPROJString().
 *
 * This function may return NULL if the object is not compatible with an
 * export to the requested type.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object (must not be NULL)
 * @param type PROJ String version.
 * @param options NULL-terminated list of strings with "KEY=VALUE" format. or
 * NULL.
 * The currently recognized option is USE_ETMERC=YES to use
 * +proj=etmerc instead of +proj=tmerc (or USE_ETMERC=NO to disable implicit
 * use of etmerc by utm conversions)
 * @return a string, or NULL in case of error.
 */
const char *proj_obj_as_proj_string(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                                    PJ_PROJ_STRING_TYPE type,
                                    const char *const *options) {
    SANITIZE_CTX(ctx);
    assert(obj);
    auto exportable =
        dynamic_cast<const IPROJStringExportable *>(obj->obj.get());
    if (!exportable) {
        proj_log_error(ctx, __FUNCTION__, "Object type not exportable to PROJ");
        return nullptr;
    }
    // Make sure that the C and C++ enumeration match
    static_assert(static_cast<int>(PJ_PROJ_5) ==
                      static_cast<int>(PROJStringFormatter::Convention::PROJ_5),
                  "");
    static_assert(static_cast<int>(PJ_PROJ_4) ==
                      static_cast<int>(PROJStringFormatter::Convention::PROJ_4),
                  "");
    // Make sure we enumerate all values. If adding a new value, as we
    // don't have a default clause, the compiler will warn.
    switch (type) {
    case PJ_PROJ_5:
    case PJ_PROJ_4:
        break;
    }
    const PROJStringFormatter::Convention convention =
        static_cast<PROJStringFormatter::Convention>(type);
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    try {
        auto formatter = PROJStringFormatter::create(convention, dbContext);
        if (options != nullptr && options[0] != nullptr) {
            if (ci_equal(options[0], "USE_ETMERC=YES")) {
                formatter->setUseETMercForTMerc(true);
            } else if (ci_equal(options[0], "USE_ETMERC=NO")) {
                formatter->setUseETMercForTMerc(false);
            }
        }
        obj->lastPROJString = exportable->exportToPROJString(formatter.get());
        return obj->lastPROJString.c_str();
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return the area of use of an object.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object (must not be NULL)
 * @param out_west_lon_degree Pointer to a double to receive the west longitude
 * (in degrees). Or NULL. If the returned value is -1000, the bounding box is
 * unknown.
 * @param out_south_lat_degree Pointer to a double to receive the south latitude
 * (in degrees). Or NULL. If the returned value is -1000, the bounding box is
 * unknown.
 * @param out_east_lon_degree Pointer to a double to receive the east longitude
 * (in degrees). Or NULL. If the returned value is -1000, the bounding box is
 * unknown.
 * @param out_north_lat_degree Pointer to a double to receive the north latitude
 * (in degrees). Or NULL. If the returned value is -1000, the bounding box is
 * unknown.
 * @param out_area_name Pointer to a string to receive the name of the area of
 * use. Or NULL. *p_area_name is valid while obj is valid itself.
 * @return TRUE in case of success, FALSE in case of error or if the area
 * of use is unknown.
 */
int proj_obj_get_area_of_use(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                             double *out_west_lon_degree,
                             double *out_south_lat_degree,
                             double *out_east_lon_degree,
                             double *out_north_lat_degree,
                             const char **out_area_name) {
    (void)ctx;
    if (out_area_name) {
        *out_area_name = nullptr;
    }
    auto objectUsage = dynamic_cast<const ObjectUsage *>(obj->obj.get());
    if (!objectUsage) {
        return false;
    }
    const auto &domains = objectUsage->domains();
    if (domains.empty()) {
        return false;
    }
    const auto &extent = domains[0]->domainOfValidity();
    if (!extent) {
        return false;
    }
    const auto &desc = extent->description();
    if (desc.has_value() && out_area_name) {
        *out_area_name = desc->c_str();
    }

    const auto &geogElements = extent->geographicElements();
    if (!geogElements.empty()) {
        auto bbox =
            dynamic_cast<const GeographicBoundingBox *>(geogElements[0].get());
        if (bbox) {
            if (out_west_lon_degree) {
                *out_west_lon_degree = bbox->westBoundLongitude();
            }
            if (out_south_lat_degree) {
                *out_south_lat_degree = bbox->southBoundLatitude();
            }
            if (out_east_lon_degree) {
                *out_east_lon_degree = bbox->eastBoundLongitude();
            }
            if (out_north_lat_degree) {
                *out_north_lat_degree = bbox->northBoundLatitude();
            }
            return true;
        }
    }
    if (out_west_lon_degree) {
        *out_west_lon_degree = -1000;
    }
    if (out_south_lat_degree) {
        *out_south_lat_degree = -1000;
    }
    if (out_east_lon_degree) {
        *out_east_lon_degree = -1000;
    }
    if (out_north_lat_degree) {
        *out_north_lat_degree = -1000;
    }
    return true;
}

// ---------------------------------------------------------------------------

static const GeodeticCRS *extractGeodeticCRS(PJ_CONTEXT *ctx, const PJ_OBJ *crs,
                                             const char *fname) {
    assert(crs);
    auto l_crs = dynamic_cast<const CRS *>(crs->obj.get());
    if (!l_crs) {
        proj_log_error(ctx, fname, "Object is not a CRS");
        return nullptr;
    }
    auto geodCRS = l_crs->extractGeodeticCRSRaw();
    if (!geodCRS) {
        proj_log_error(ctx, fname, "CRS has no geodetic CRS");
    }
    return geodCRS;
}

// ---------------------------------------------------------------------------

/** \brief Get the geodeticCRS / geographicCRS from a CRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type CRS (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_crs_get_geodetic_crs(PJ_CONTEXT *ctx, const PJ_OBJ *crs) {
    SANITIZE_CTX(ctx);
    auto geodCRS = extractGeodeticCRS(ctx, crs, __FUNCTION__);
    if (!geodCRS) {
        return nullptr;
    }
    return PJ_OBJ::create(NN_NO_CHECK(nn_dynamic_pointer_cast<IdentifiedObject>(
        geodCRS->shared_from_this())));
}

// ---------------------------------------------------------------------------

/** \brief Get a CRS component from a CompoundCRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type CRS (must not be NULL)
 * @param index Index of the CRS component (typically 0 = horizontal, 1 =
 * vertical)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_crs_get_sub_crs(PJ_CONTEXT *ctx, const PJ_OBJ *crs,
                                 int index) {
    SANITIZE_CTX(ctx);
    assert(crs);
    auto l_crs = dynamic_cast<CompoundCRS *>(crs->obj.get());
    if (!l_crs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a CompoundCRS");
        return nullptr;
    }
    const auto &components = l_crs->componentReferenceSystems();
    if (static_cast<size_t>(index) >= components.size()) {
        return nullptr;
    }
    return PJ_OBJ::create(components[index]);
}

// ---------------------------------------------------------------------------

/** \brief Returns a BoundCRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param base_crs Base CRS (must not be NULL)
 * @param hub_crs Hub CRS (must not be NULL)
 * @param transformation Transformation (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_crs_create_bound_crs(PJ_CONTEXT *ctx, const PJ_OBJ *base_crs,
                                      const PJ_OBJ *hub_crs,
                                      const PJ_OBJ *transformation) {
    SANITIZE_CTX(ctx);
    assert(base_crs);
    assert(hub_crs);
    assert(transformation);
    auto l_base_crs = util::nn_dynamic_pointer_cast<CRS>(base_crs->obj);
    if (!l_base_crs) {
        proj_log_error(ctx, __FUNCTION__, "base_crs is not a CRS");
        return nullptr;
    }
    auto l_hub_crs = util::nn_dynamic_pointer_cast<CRS>(hub_crs->obj);
    if (!l_hub_crs) {
        proj_log_error(ctx, __FUNCTION__, "hub_crs is not a CRS");
        return nullptr;
    }
    auto l_transformation =
        util::nn_dynamic_pointer_cast<Transformation>(transformation->obj);
    if (!l_transformation) {
        proj_log_error(ctx, __FUNCTION__, "transformation is not a CRS");
        return nullptr;
    }
    try {
        return PJ_OBJ::create(BoundCRS::create(NN_NO_CHECK(l_base_crs),
                                               NN_NO_CHECK(l_hub_crs),
                                               NN_NO_CHECK(l_transformation)));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Returns potentially
 * a BoundCRS, with a transformation to EPSG:4326, wrapping this CRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * This is the same as method
 * osgeo::proj::crs::CRS::createBoundCRSToWGS84IfPossible()
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type CRS (must not be NULL)
 * @param options null-terminated list of options, or NULL. Currently
 * supported options are:
 * <ul>
 * <li>ALLOW_INTERMEDIATE_CRS=YES/NO. Defaults to NO. When set to YES,
 * intermediate CRS may be considered when computing the possible
 * tranformations. Slower.</li>
 * </ul>
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_crs_create_bound_crs_to_WGS84(PJ_CONTEXT *ctx,
                                               const PJ_OBJ *crs,
                                               const char *const *options) {
    SANITIZE_CTX(ctx);
    assert(crs);
    auto l_crs = dynamic_cast<const CRS *>(crs->obj.get());
    if (!l_crs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a CRS");
        return nullptr;
    }
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    try {
        bool allowIntermediateCRS = false;
        for (auto iter = options; iter && iter[0]; ++iter) {
            const char *value;
            if ((value = getOptionValue(*iter, "ALLOW_INTERMEDIATE_CRS="))) {
                allowIntermediateCRS = ci_equal(value, "YES");
            } else {
                std::string msg("Unknown option :");
                msg += *iter;
                proj_log_error(ctx, __FUNCTION__, msg.c_str());
                return nullptr;
            }
        }
        return PJ_OBJ::create(l_crs->createBoundCRSToWGS84IfPossible(
            dbContext, allowIntermediateCRS));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Get the ellipsoid from a CRS or a GeodeticReferenceFrame.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Objet of type CRS or GeodeticReferenceFrame (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_get_ellipsoid(PJ_CONTEXT *ctx, const PJ_OBJ *obj) {
    SANITIZE_CTX(ctx);
    auto ptr = obj->obj.get();
    if (dynamic_cast<const CRS *>(ptr)) {
        auto geodCRS = extractGeodeticCRS(ctx, obj, __FUNCTION__);
        if (geodCRS) {
            return PJ_OBJ::create(geodCRS->ellipsoid());
        }
    } else {
        auto datum = dynamic_cast<const GeodeticReferenceFrame *>(ptr);
        if (datum) {
            return PJ_OBJ::create(datum->ellipsoid());
        }
    }
    proj_log_error(ctx, __FUNCTION__,
                   "Object is not a CRS or GeodeticReferenceFrame");
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Get the horizontal datum from a CRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type CRS (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_crs_get_horizontal_datum(PJ_CONTEXT *ctx, const PJ_OBJ *crs) {
    SANITIZE_CTX(ctx);
    auto geodCRS = extractGeodeticCRS(ctx, crs, __FUNCTION__);
    if (!geodCRS) {
        return nullptr;
    }
    const auto &datum = geodCRS->datum();
    if (datum) {
        return PJ_OBJ::create(NN_NO_CHECK(datum));
    }

    const auto &datumEnsemble = geodCRS->datumEnsemble();
    if (datumEnsemble) {
        return PJ_OBJ::create(NN_NO_CHECK(datumEnsemble));
    }
    proj_log_error(ctx, __FUNCTION__, "CRS has no datum");
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return ellipsoid parameters.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param ellipsoid Object of type Ellipsoid (must not be NULL)
 * @param out_semi_major_metre Pointer to a value to store the semi-major axis
 * in
 * metre. or NULL
 * @param out_semi_minor_metre Pointer to a value to store the semi-minor axis
 * in
 * metre. or NULL
 * @param out_is_semi_minor_computed Pointer to a boolean value to indicate if
 * the
 * semi-minor value was computed. If FALSE, its value comes from the
 * definition. or NULL
 * @param out_inv_flattening Pointer to a value to store the inverse
 * flattening. or NULL
 * @return TRUE in case of success.
 */
int proj_obj_ellipsoid_get_parameters(PJ_CONTEXT *ctx, const PJ_OBJ *ellipsoid,
                                      double *out_semi_major_metre,
                                      double *out_semi_minor_metre,
                                      int *out_is_semi_minor_computed,
                                      double *out_inv_flattening) {
    SANITIZE_CTX(ctx);
    assert(ellipsoid);
    auto l_ellipsoid = dynamic_cast<const Ellipsoid *>(ellipsoid->obj.get());
    if (!l_ellipsoid) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a Ellipsoid");
        return FALSE;
    }

    if (out_semi_major_metre) {
        *out_semi_major_metre = l_ellipsoid->semiMajorAxis().getSIValue();
    }
    if (out_semi_minor_metre) {
        *out_semi_minor_metre =
            l_ellipsoid->computeSemiMinorAxis().getSIValue();
    }
    if (out_is_semi_minor_computed) {
        *out_is_semi_minor_computed =
            !(l_ellipsoid->semiMinorAxis().has_value());
    }
    if (out_inv_flattening) {
        *out_inv_flattening = l_ellipsoid->computedInverseFlattening();
    }
    return TRUE;
}

// ---------------------------------------------------------------------------

/** \brief Get the prime meridian of a CRS or a GeodeticReferenceFrame.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Objet of type CRS or GeodeticReferenceFrame (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */

PJ_OBJ *proj_obj_get_prime_meridian(PJ_CONTEXT *ctx, const PJ_OBJ *obj) {
    SANITIZE_CTX(ctx);
    auto ptr = obj->obj.get();
    if (dynamic_cast<CRS *>(ptr)) {
        auto geodCRS = extractGeodeticCRS(ctx, obj, __FUNCTION__);
        if (geodCRS) {
            return PJ_OBJ::create(geodCRS->primeMeridian());
        }
    } else {
        auto datum = dynamic_cast<const GeodeticReferenceFrame *>(ptr);
        if (datum) {
            return PJ_OBJ::create(datum->primeMeridian());
        }
    }
    proj_log_error(ctx, __FUNCTION__,
                   "Object is not a CRS or GeodeticReferenceFrame");
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return prime meridian parameters.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param prime_meridian Object of type PrimeMeridian (must not be NULL)
 * @param out_longitude Pointer to a value to store the longitude of the prime
 * meridian, in its native unit. or NULL
 * @param out_unit_conv_factor Pointer to a value to store the conversion
 * factor of the prime meridian longitude unit to radian. or NULL
 * @param out_unit_name Pointer to a string value to store the unit name.
 * or NULL
 * @return TRUE in case of success.
 */
int proj_obj_prime_meridian_get_parameters(PJ_CONTEXT *ctx,
                                           const PJ_OBJ *prime_meridian,
                                           double *out_longitude,
                                           double *out_unit_conv_factor,
                                           const char **out_unit_name) {
    SANITIZE_CTX(ctx);
    assert(prime_meridian);
    auto l_pm = dynamic_cast<const PrimeMeridian *>(prime_meridian->obj.get());
    if (!l_pm) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a PrimeMeridian");
        return false;
    }
    const auto &longitude = l_pm->longitude();
    if (out_longitude) {
        *out_longitude = longitude.value();
    }
    const auto &unit = longitude.unit();
    if (out_unit_conv_factor) {
        *out_unit_conv_factor = unit.conversionToSI();
    }
    if (out_unit_name) {
        *out_unit_name = unit.name().c_str();
    }
    return true;
}

// ---------------------------------------------------------------------------

/** \brief Return the base CRS of a BoundCRS or a DerivedCRS/ProjectedCRS, or
 * the source CRS of a CoordinateOperation.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Objet of type BoundCRS or CoordinateOperation (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error, or missing source CRS.
 */
PJ_OBJ *proj_obj_get_source_crs(PJ_CONTEXT *ctx, const PJ_OBJ *obj) {
    SANITIZE_CTX(ctx);
    assert(obj);
    auto ptr = obj->obj.get();
    auto boundCRS = dynamic_cast<const BoundCRS *>(ptr);
    if (boundCRS) {
        return PJ_OBJ::create(boundCRS->baseCRS());
    }
    auto derivedCRS = dynamic_cast<const DerivedCRS *>(ptr);
    if (derivedCRS) {
        return PJ_OBJ::create(derivedCRS->baseCRS());
    }
    auto co = dynamic_cast<const CoordinateOperation *>(ptr);
    if (co) {
        auto sourceCRS = co->sourceCRS();
        if (sourceCRS) {
            return PJ_OBJ::create(NN_NO_CHECK(sourceCRS));
        }
        return nullptr;
    }
    proj_log_error(ctx, __FUNCTION__,
                   "Object is not a BoundCRS or a CoordinateOperation");
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return the hub CRS of a BoundCRS or the target CRS of a
 * CoordinateOperation.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Objet of type BoundCRS or CoordinateOperation (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error, or missing target CRS.
 */
PJ_OBJ *proj_obj_get_target_crs(PJ_CONTEXT *ctx, const PJ_OBJ *obj) {
    SANITIZE_CTX(ctx);
    assert(obj);
    auto ptr = obj->obj.get();
    auto boundCRS = dynamic_cast<const BoundCRS *>(ptr);
    if (boundCRS) {
        return PJ_OBJ::create(boundCRS->hubCRS());
    }
    auto co = dynamic_cast<const CoordinateOperation *>(ptr);
    if (co) {
        auto targetCRS = co->targetCRS();
        if (targetCRS) {
            return PJ_OBJ::create(NN_NO_CHECK(targetCRS));
        }
        return nullptr;
    }
    proj_log_error(ctx, __FUNCTION__,
                   "Object is not a BoundCRS or a CoordinateOperation");
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Identify the CRS with reference CRSs.
 *
 * The candidate CRSs are either hard-coded, or looked in the database when
 * authorityFactory is not null.
 *
 * The method returns a list of matching reference CRS, and the percentage
 * (0-100) of confidence in the match.
 * 100% means that the name of the reference entry
 * perfectly matches the CRS name, and both are equivalent. In which case a
 * single result is returned.
 * 90% means that CRS are equivalent, but the names are not exactly the same.
 * 70% means that CRS are equivalent), but the names do not match at all.
 * 25% means that the CRS are not equivalent, but there is some similarity in
 * the names.
 * Other confidence values may be returned by some specialized implementations.
 *
 * This is implemented for GeodeticCRS, ProjectedCRS, VerticalCRS and
 * CompoundCRS.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object of type CRS. Must not be NULL
 * @param auth_name Authority name, or NULL for all authorities
 * @param options Placeholder for future options. Should be set to NULL.
 * @param out_confidence Output parameter. Pointer to an array of integers that
 * will be allocated by the function and filled with the confidence values
 * (0-100). There are as many elements in this array as
 * proj_obj_list_get_count()
 * returns on the return value of this function. *confidence should be
 * released with proj_free_int_list().
 * @return a list of matching reference CRS, or nullptr in case of error.
 */
PJ_OBJ_LIST *proj_obj_identify(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                               const char *auth_name,
                               const char *const *options,
                               int **out_confidence) {
    SANITIZE_CTX(ctx);
    assert(obj);
    (void)options;
    if (out_confidence) {
        *out_confidence = nullptr;
    }
    auto ptr = obj->obj.get();
    auto crs = dynamic_cast<const CRS *>(ptr);
    if (!crs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a CRS");
    } else {
        int *confidenceTemp = nullptr;
        try {
            auto factory = AuthorityFactory::create(getDBcontext(ctx),
                                                    auth_name ? auth_name : "");
            auto res = crs->identify(factory);
            std::vector<IdentifiedObjectNNPtr> objects;
            confidenceTemp = out_confidence ? new int[res.size()] : nullptr;
            size_t i = 0;
            for (const auto &pair : res) {
                objects.push_back(pair.first);
                if (confidenceTemp) {
                    confidenceTemp[i] = pair.second;
                    ++i;
                }
            }
            auto ret = internal::make_unique<PJ_OBJ_LIST>(std::move(objects));
            if (out_confidence) {
                *out_confidence = confidenceTemp;
                confidenceTemp = nullptr;
            }
            return ret.release();
        } catch (const std::exception &e) {
            delete[] confidenceTemp;
            proj_log_error(ctx, __FUNCTION__, e.what());
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Free an array of integer. */
void proj_free_int_list(int *list) { delete[] list; }

// ---------------------------------------------------------------------------

static PROJ_STRING_LIST set_to_string_list(std::set<std::string> &&set) {
    auto ret = new char *[set.size() + 1];
    size_t i = 0;
    for (const auto &str : set) {
        ret[i] = new char[str.size() + 1];
        std::memcpy(ret[i], str.c_str(), str.size() + 1);
        i++;
    }
    ret[i] = nullptr;
    return ret;
}

// ---------------------------------------------------------------------------

/** \brief Return the list of authorities used in the database.
 *
 * The returned list is NULL terminated and must be freed with
 * proj_free_string_list().
 *
 * @param ctx PROJ context, or NULL for default context
 *
 * @return a NULL terminated list of NUL-terminated strings that must be
 * freed with proj_free_string_list(), or NULL in case of error.
 */
PROJ_STRING_LIST proj_get_authorities_from_database(PJ_CONTEXT *ctx) {
    SANITIZE_CTX(ctx);
    try {
        return set_to_string_list(getDBcontext(ctx)->getAuthorities());
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Returns the set of authority codes of the given object type.
 *
 * The returned list is NULL terminated and must be freed with
 * proj_free_string_list().
 *
 * @param ctx PROJ context, or NULL for default context.
 * @param auth_name Authority name (must not be NULL)
 * @param type Object type.
 * @param allow_deprecated whether we should return deprecated objects as well.
 *
 * @return a NULL terminated list of NUL-terminated strings that must be
 * freed with proj_free_string_list(), or NULL in case of error.
 */
PROJ_STRING_LIST proj_get_codes_from_database(PJ_CONTEXT *ctx,
                                              const char *auth_name,
                                              PJ_OBJ_TYPE type,
                                              int allow_deprecated) {
    assert(auth_name);
    SANITIZE_CTX(ctx);
    try {
        auto factory = AuthorityFactory::create(getDBcontext(ctx), auth_name);
        bool valid = false;
        auto typeInternal = convertPJObjectTypeToObjectType(type, valid);
        if (!valid) {
            return nullptr;
        }
        return set_to_string_list(
            factory->getAuthorityCodes(typeInternal, allow_deprecated != 0));

    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** Free a list of NULL terminated strings. */
void proj_free_string_list(PROJ_STRING_LIST list) {
    if (list) {
        for (size_t i = 0; list[i] != nullptr; i++) {
            delete[] list[i];
        }
        delete[] list;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return the Conversion of a DerivedCRS (such as a ProjectedCRS),
 * or the Transformation from the baseCRS to the hubCRS of a BoundCRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type DerivedCRS or BoundCRSs (must not be NULL)
 * @return Object of type SingleOperation that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_crs_get_coordoperation(PJ_CONTEXT *ctx, const PJ_OBJ *crs) {
    SANITIZE_CTX(ctx);
    assert(crs);
    SingleOperationPtr co;

    auto derivedCRS = dynamic_cast<const DerivedCRS *>(crs->obj.get());
    if (derivedCRS) {
        co = derivedCRS->derivingConversion().as_nullable();
    } else {
        auto boundCRS = dynamic_cast<const BoundCRS *>(crs->obj.get());
        if (boundCRS) {
            co = boundCRS->transformation().as_nullable();
        } else {
            proj_log_error(ctx, __FUNCTION__,
                           "Object is not a DerivedCRS or BoundCRS");
            return nullptr;
        }
    }

    return PJ_OBJ::create(NN_NO_CHECK(co));
}

// ---------------------------------------------------------------------------

/** \brief Return informatin on the operation method of the SingleOperation.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type SingleOperation (typically a Conversion
 * or Transformation) (must not be NULL)
 * @param out_method_name Pointer to a string value to store the method
 * (projection) name. or NULL
 * @param out_method_auth_name Pointer to a string value to store the method
 * authority name. or NULL
 * @param out_method_code Pointer to a string value to store the method
 * code. or NULL
 * @return TRUE in case of success.
 */
int proj_coordoperation_get_method_info(PJ_CONTEXT *ctx,
                                        const PJ_OBJ *coordoperation,
                                        const char **out_method_name,
                                        const char **out_method_auth_name,
                                        const char **out_method_code) {
    SANITIZE_CTX(ctx);
    assert(coordoperation);

    auto singleOp =
        dynamic_cast<const SingleOperation *>(coordoperation->obj.get());
    if (!singleOp) {
        proj_log_error(ctx, __FUNCTION__,
                       "Object is not a DerivedCRS or BoundCRS");
        return false;
    }

    const auto &method = singleOp->method();
    const auto &method_ids = method->identifiers();
    if (out_method_name) {
        *out_method_name = method->name()->description()->c_str();
    }
    if (out_method_auth_name) {
        if (!method_ids.empty()) {
            *out_method_auth_name = method_ids[0]->codeSpace()->c_str();
        } else {
            *out_method_auth_name = nullptr;
        }
    }
    if (out_method_code) {
        if (!method_ids.empty()) {
            *out_method_code = method_ids[0]->code().c_str();
        } else {
            *out_method_code = nullptr;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress
static PropertyMap createPropertyMapName(const char *c_name) {
    std::string name(c_name ? c_name : "unnamed");
    PropertyMap properties;
    if (ends_with(name, " (deprecated)")) {
        name.resize(name.size() - strlen(" (deprecated)"));
        properties.set(common::IdentifiedObject::DEPRECATED_KEY, true);
    }
    return properties.set(common::IdentifiedObject::NAME_KEY, name);
}

// ---------------------------------------------------------------------------

static UnitOfMeasure createLinearUnit(const char *name, double convFactor) {
    return name == nullptr
               ? UnitOfMeasure::METRE
               : UnitOfMeasure(name, convFactor, UnitOfMeasure::Type::LINEAR);
}

// ---------------------------------------------------------------------------

static UnitOfMeasure createAngularUnit(const char *name, double convFactor) {
    return name ? (ci_equal(name, "degree")
                       ? UnitOfMeasure::DEGREE
                       : ci_equal(name, "grad")
                             ? UnitOfMeasure::GRAD
                             : UnitOfMeasure(name, convFactor,
                                             UnitOfMeasure::Type::ANGULAR))
                : UnitOfMeasure::DEGREE;
}

// ---------------------------------------------------------------------------

static GeodeticReferenceFrameNNPtr createGeodeticReferenceFrame(
    PJ_CONTEXT *ctx, const char *datum_name, const char *ellps_name,
    double semi_major_metre, double inv_flattening,
    const char *prime_meridian_name, double prime_meridian_offset,
    const char *angular_units, double angular_units_conv) {
    const UnitOfMeasure angUnit(
        createAngularUnit(angular_units, angular_units_conv));
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    auto body = Ellipsoid::guessBodyName(dbContext, semi_major_metre);
    auto ellpsName = createPropertyMapName(ellps_name);
    auto ellps = inv_flattening != 0.0
                     ? Ellipsoid::createFlattenedSphere(
                           ellpsName, Length(semi_major_metre),
                           Scale(inv_flattening), body)
                     : Ellipsoid::createSphere(ellpsName,
                                               Length(semi_major_metre), body);
    auto pm = PrimeMeridian::create(
        PropertyMap().set(
            common::IdentifiedObject::NAME_KEY,
            prime_meridian_name
                ? prime_meridian_name
                : prime_meridian_offset == 0.0
                      ? (ellps->celestialBody() == Ellipsoid::EARTH
                             ? "Greenwich"
                             : "Reference meridian")
                      : "unnamed"),
        Angle(prime_meridian_offset, angUnit));

    std::string datumName(datum_name ? datum_name : "unnamed");
    if (datumName == "WGS_1984") {
        datumName = GeodeticReferenceFrame::EPSG_6326->nameStr();
    } else if (datumName.find('_') != std::string::npos) {
        // Likely coming from WKT1
        if (dbContext) {
            auto authFactory =
                AuthorityFactory::create(NN_NO_CHECK(dbContext), std::string());
            auto res = authFactory->createObjectsFromName(
                datumName,
                {AuthorityFactory::ObjectType::GEODETIC_REFERENCE_FRAME}, true,
                1);
            if (!res.empty()) {
                const auto &refDatum = res.front();
                if (metadata::Identifier::isEquivalentName(
                        datumName.c_str(), refDatum->nameStr().c_str())) {
                    datumName = refDatum->nameStr();
                }
            } else {
                std::string outTableName;
                std::string authNameFromAlias;
                std::string codeFromAlias;
                auto officialName = authFactory->getOfficialNameFromAlias(
                    datumName, "geodetic_datum", std::string(), true,
                    outTableName, authNameFromAlias, codeFromAlias);
                if (!officialName.empty()) {
                    datumName = officialName;
                }
            }
        }
    }

    return GeodeticReferenceFrame::create(
        createPropertyMapName(datumName.c_str()), ellps,
        util::optional<std::string>(), pm);
}

//! @endcond

// ---------------------------------------------------------------------------

/** \brief Create a GeographicCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name Name of the GeographicCRS. Or NULL
 * @param datum_name Name of the GeodeticReferenceFrame. Or NULL
 * @param ellps_name Name of the Ellipsoid. Or NULL
 * @param semi_major_metre Ellipsoid semi-major axis, in metres.
 * @param inv_flattening Ellipsoid inverse flattening. Or 0 for a sphere.
 * @param prime_meridian_name Name of the PrimeMeridian. Or NULL
 * @param prime_meridian_offset Offset of the prime meridian, expressed in the
 * specified angular units.
 * @param pm_angular_units Name of the angular units. Or NULL for Degree
 * @param pm_angular_units_conv Conversion factor from the angular unit to
 * radian.
 * Or
 * 0 for Degree if pm_angular_units == NULL. Otherwise should be not NULL
 * @param ellipsoidal_cs Coordinate system. Must not be NULL.
 *
 * @return Object of type GeographicCRS that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_create_geographic_crs(
    PJ_CONTEXT *ctx, const char *crs_name, const char *datum_name,
    const char *ellps_name, double semi_major_metre, double inv_flattening,
    const char *prime_meridian_name, double prime_meridian_offset,
    const char *pm_angular_units, double pm_angular_units_conv,
    PJ_OBJ *ellipsoidal_cs) {

    SANITIZE_CTX(ctx);
    auto cs = util::nn_dynamic_pointer_cast<EllipsoidalCS>(ellipsoidal_cs->obj);
    if (!cs) {
        return nullptr;
    }
    try {
        auto datum = createGeodeticReferenceFrame(
            ctx, datum_name, ellps_name, semi_major_metre, inv_flattening,
            prime_meridian_name, prime_meridian_offset, pm_angular_units,
            pm_angular_units_conv);
        auto geogCRS = GeographicCRS::create(createPropertyMapName(crs_name),
                                             datum, NN_NO_CHECK(cs));
        return PJ_OBJ::create(geogCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Create a GeographicCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name Name of the GeographicCRS. Or NULL
 * @param datum Datum. Must not be NULL.
 * @param ellipsoidal_cs Coordinate system. Must not be NULL.
 *
 * @return Object of type GeographicCRS that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_create_geographic_crs_from_datum(PJ_CONTEXT *ctx,
                                                  const char *crs_name,
                                                  PJ_OBJ *datum,
                                                  PJ_OBJ *ellipsoidal_cs) {

    SANITIZE_CTX(ctx);
    auto l_datum =
        util::nn_dynamic_pointer_cast<GeodeticReferenceFrame>(datum->obj);
    if (!l_datum) {
        proj_log_error(ctx, __FUNCTION__,
                       "datum is not a GeodeticReferenceFrame");
        return nullptr;
    }
    auto cs = util::nn_dynamic_pointer_cast<EllipsoidalCS>(ellipsoidal_cs->obj);
    if (!cs) {
        return nullptr;
    }
    try {
        auto geogCRS =
            GeographicCRS::create(createPropertyMapName(crs_name),
                                  NN_NO_CHECK(l_datum), NN_NO_CHECK(cs));
        return PJ_OBJ::create(geogCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Create a GeodeticCRS of geocentric type.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name Name of the GeographicCRS. Or NULL
 * @param datum_name Name of the GeodeticReferenceFrame. Or NULL
 * @param ellps_name Name of the Ellipsoid. Or NULL
 * @param semi_major_metre Ellipsoid semi-major axis, in metres.
 * @param inv_flattening Ellipsoid inverse flattening. Or 0 for a sphere.
 * @param prime_meridian_name Name of the PrimeMeridian. Or NULL
 * @param prime_meridian_offset Offset of the prime meridian, expressed in the
 * specified angular units.
 * @param angular_units Name of the angular units. Or NULL for Degree
 * @param angular_units_conv Conversion factor from the angular unit to radian.
 * Or
 * 0 for Degree if angular_units == NULL. Otherwise should be not NULL
 * @param linear_units Name of the linear units. Or NULL for Metre
 * @param linear_units_conv Conversion factor from the linear unit to metre. Or
 * 0 for Metre if linear_units == NULL. Otherwise should be not NULL
 *
 * @return Object of type GeodeticCRS that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_create_geocentric_crs(
    PJ_CONTEXT *ctx, const char *crs_name, const char *datum_name,
    const char *ellps_name, double semi_major_metre, double inv_flattening,
    const char *prime_meridian_name, double prime_meridian_offset,
    const char *angular_units, double angular_units_conv,
    const char *linear_units, double linear_units_conv) {

    SANITIZE_CTX(ctx);
    try {
        const UnitOfMeasure linearUnit(
            createLinearUnit(linear_units, linear_units_conv));
        auto datum = createGeodeticReferenceFrame(
            ctx, datum_name, ellps_name, semi_major_metre, inv_flattening,
            prime_meridian_name, prime_meridian_offset, angular_units,
            angular_units_conv);

        auto geodCRS =
            GeodeticCRS::create(createPropertyMapName(crs_name), datum,
                                cs::CartesianCS::createGeocentric(linearUnit));
        return PJ_OBJ::create(geodCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Create a GeodeticCRS of geocentric type.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name Name of the GeographicCRS. Or NULL
 * @param datum Datum. Must not be NULL.
 * @param linear_units Name of the linear units. Or NULL for Metre
 * @param linear_units_conv Conversion factor from the linear unit to metre. Or
 * 0 for Metre if linear_units == NULL. Otherwise should be not NULL
 *
 * @return Object of type GeodeticCRS that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_create_geocentric_crs_from_datum(PJ_CONTEXT *ctx,
                                                  const char *crs_name,
                                                  const PJ_OBJ *datum,
                                                  const char *linear_units,
                                                  double linear_units_conv) {
    SANITIZE_CTX(ctx);
    try {
        const UnitOfMeasure linearUnit(
            createLinearUnit(linear_units, linear_units_conv));
        auto l_datum =
            util::nn_dynamic_pointer_cast<GeodeticReferenceFrame>(datum->obj);
        if (!l_datum) {
            proj_log_error(ctx, __FUNCTION__,
                           "datum is not a GeodeticReferenceFrame");
            return nullptr;
        }
        auto geodCRS = GeodeticCRS::create(
            createPropertyMapName(crs_name), NN_NO_CHECK(l_datum),
            cs::CartesianCS::createGeocentric(linearUnit));
        return PJ_OBJ::create(geodCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Create a VerticalCRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name Name of the GeographicCRS. Or NULL
 * @param datum_name Name of the VerticalReferenceFrame. Or NULL
 * @param linear_units Name of the linear units. Or NULL for Metre
 * @param linear_units_conv Conversion factor from the linear unit to metre. Or
 * 0 for Metre if linear_units == NULL. Otherwise should be not NULL
 *
 * @return Object of type VerticalCRS that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_create_vertical_crs(PJ_CONTEXT *ctx, const char *crs_name,
                                     const char *datum_name,
                                     const char *linear_units,
                                     double linear_units_conv) {

    SANITIZE_CTX(ctx);
    try {
        const UnitOfMeasure linearUnit(
            createLinearUnit(linear_units, linear_units_conv));
        auto datum =
            VerticalReferenceFrame::create(createPropertyMapName(datum_name));
        auto vertCRS = VerticalCRS::create(
            createPropertyMapName(crs_name), datum,
            cs::VerticalCS::createGravityRelatedHeight(linearUnit));
        return PJ_OBJ::create(vertCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Create a CompoundCRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name Name of the GeographicCRS. Or NULL
 * @param horiz_crs Horizontal CRS. must not be NULL.
 * @param vert_crs Vertical CRS. must not be NULL.
 *
 * @return Object of type CompoundCRS that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_create_compound_crs(PJ_CONTEXT *ctx, const char *crs_name,
                                     PJ_OBJ *horiz_crs, PJ_OBJ *vert_crs) {

    assert(horiz_crs);
    assert(vert_crs);
    SANITIZE_CTX(ctx);
    auto l_horiz_crs = util::nn_dynamic_pointer_cast<CRS>(horiz_crs->obj);
    if (!l_horiz_crs) {
        return nullptr;
    }
    auto l_vert_crs = util::nn_dynamic_pointer_cast<CRS>(vert_crs->obj);
    if (!l_vert_crs) {
        return nullptr;
    }
    try {
        auto compoundCRS = CompoundCRS::create(
            createPropertyMapName(crs_name),
            {NN_NO_CHECK(l_horiz_crs), NN_NO_CHECK(l_vert_crs)});
        return PJ_OBJ::create(compoundCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return a copy of the object with its name changed
 *
 * Currently, only implemented on CRS objects.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object of type CRS. Must not be NULL
 * @param name New name. Must not be NULL
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ PROJ_DLL *proj_obj_alter_name(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                                     const char *name) {
    SANITIZE_CTX(ctx);
    auto crs = dynamic_cast<const CRS *>(obj->obj.get());
    if (!crs) {
        return nullptr;
    }
    try {
        return PJ_OBJ::create(crs->alterName(name));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Return a copy of the CRS with its geodetic CRS changed
 *
 * Currently, when obj is a GeodeticCRS, it returns a clone of new_geod_crs
 * When obj is a ProjectedCRS, it replaces its base CRS with new_geod_crs.
 * When obj is a CompoundCRS, it replaces the GeodeticCRS part of the horizontal
 * CRS with new_geod_crs.
 * In other cases, it returns a clone of obj.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object of type CRS. Must not be NULL
 * @param new_geod_crs Object of type GeodeticCRS. Must not be NULL
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_crs_alter_geodetic_crs(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                                        const PJ_OBJ *new_geod_crs) {
    SANITIZE_CTX(ctx);
    auto l_new_geod_crs =
        util::nn_dynamic_pointer_cast<GeodeticCRS>(new_geod_crs->obj);
    if (!l_new_geod_crs) {
        proj_log_error(ctx, __FUNCTION__, "new_geod_crs is not a GeodeticCRS");
        return nullptr;
    }

    auto crs = dynamic_cast<const CRS *>(obj->obj.get());
    if (!crs) {
        proj_log_error(ctx, __FUNCTION__, "obj is not a CRS");
        return nullptr;
    }

    try {
        return PJ_OBJ::create(
            crs->alterGeodeticCRS(NN_NO_CHECK(l_new_geod_crs)));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return a copy of the CRS with its angular units changed
 *
 * The CRS must be or contain a GeographicCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object of type CRS. Must not be NULL
 * @param angular_units Name of the angular units. Or NULL for Degree
 * @param angular_units_conv Conversion factor from the angular unit to radian.
 * Or
 * 0 for Degree if angular_units == NULL. Otherwise should be not NULL
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_crs_alter_cs_angular_unit(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                                           const char *angular_units,
                                           double angular_units_conv) {

    SANITIZE_CTX(ctx);
    auto geodCRS = proj_obj_crs_get_geodetic_crs(ctx, obj);
    if (!geodCRS) {
        return nullptr;
    }
    auto geogCRS = dynamic_cast<const GeographicCRS *>(geodCRS->obj.get());
    if (!geogCRS) {
        proj_obj_unref(geodCRS);
        return nullptr;
    }

    PJ_OBJ *geogCRSAltered = nullptr;
    try {
        const UnitOfMeasure angUnit(
            createAngularUnit(angular_units, angular_units_conv));
        geogCRSAltered = PJ_OBJ::create(GeographicCRS::create(
            createPropertyMapName(proj_obj_get_name(geodCRS)), geogCRS->datum(),
            geogCRS->datumEnsemble(),
            geogCRS->coordinateSystem()->alterAngularUnit(angUnit)));
        proj_obj_unref(geodCRS);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        proj_obj_unref(geodCRS);
        return nullptr;
    }

    auto ret = proj_obj_crs_alter_geodetic_crs(ctx, obj, geogCRSAltered);
    proj_obj_unref(geogCRSAltered);
    return ret;
}

// ---------------------------------------------------------------------------

/** \brief Return a copy of the CRS with the linear units of its coordinate
 * system changed
 *
 * The CRS must be or contain a ProjectedCRS, VerticalCRS or a GeocentricCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object of type CRS. Must not be NULL
 * @param linear_units Name of the linear units. Or NULL for Metre
 * @param linear_units_conv Conversion factor from the linear unit to metre. Or
 * 0 for Metre if linear_units == NULL. Otherwise should be not NULL
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_crs_alter_cs_linear_unit(PJ_CONTEXT *ctx, const PJ_OBJ *obj,
                                          const char *linear_units,
                                          double linear_units_conv) {
    SANITIZE_CTX(ctx);
    auto crs = dynamic_cast<const CRS *>(obj->obj.get());
    if (!crs) {
        return nullptr;
    }

    try {
        const UnitOfMeasure linearUnit(
            createLinearUnit(linear_units, linear_units_conv));
        return PJ_OBJ::create(crs->alterCSLinearUnit(linearUnit));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return a copy of the CRS with the lineaer units of the parameters
 * of its conversion modified.
 *
 * The CRS must be or contain a ProjectedCRS, VerticalCRS or a GeocentricCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param obj Object of type ProjectedCRS. Must not be NULL
 * @param linear_units Name of the linear units. Or NULL for Metre
 * @param linear_units_conv Conversion factor from the linear unit to metre. Or
 * 0 for Metre if linear_units == NULL. Otherwise should be not NULL
 * @param convert_to_new_unit TRUE if exisiting values should be converted from
 * their current unit to the new unit. If FALSE, their value will be left
 * unchanged and the unit overriden (so the resulting CRS will not be
 * equivalent to the original one for reprojection purposes).
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_crs_alter_parameters_linear_unit(PJ_CONTEXT *ctx,
                                                  const PJ_OBJ *obj,
                                                  const char *linear_units,
                                                  double linear_units_conv,
                                                  int convert_to_new_unit) {
    SANITIZE_CTX(ctx);
    auto crs = dynamic_cast<const ProjectedCRS *>(obj->obj.get());
    if (!crs) {
        return nullptr;
    }

    try {
        const UnitOfMeasure linearUnit(
            createLinearUnit(linear_units, linear_units_conv));
        return PJ_OBJ::create(crs->alterParametersLinearUnit(
            linearUnit, convert_to_new_unit == TRUE));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Instanciate a EngineeringCRS with just a name
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name CRS name. Or NULL.
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ PROJ_DLL *proj_obj_create_engineering_crs(PJ_CONTEXT *ctx,
                                                 const char *crs_name) {
    SANITIZE_CTX(ctx);
    try {
        return PJ_OBJ::create(EngineeringCRS::create(
            createPropertyMapName(crs_name),
            EngineeringDatum::create(PropertyMap()),
            CartesianCS::createEastingNorthing(UnitOfMeasure::METRE)));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Instanciate a Conversion
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param name Conversion name. Or NULL.
 * @param auth_name Conversion authority name. Or NULL.
 * @param code Conversion code. Or NULL.
 * @param method_name Method name. Or NULL.
 * @param method_auth_name Method authority name. Or NULL.
 * @param method_code Method code. Or NULL.
 * @param param_count Number of parameters (size of params argument)
 * @param params Parameter descriptions (array of size param_count)
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */

PJ_OBJ *proj_obj_create_conversion(PJ_CONTEXT *ctx, const char *name,
                                   const char *auth_name, const char *code,
                                   const char *method_name,
                                   const char *method_auth_name,
                                   const char *method_code, int param_count,
                                   const PJ_PARAM_DESCRIPTION *params) {
    SANITIZE_CTX(ctx);
    try {
        PropertyMap propConv;
        propConv.set(common::IdentifiedObject::NAME_KEY,
                     name ? name : "unnamed");
        if (auth_name && code) {
            propConv.set(metadata::Identifier::CODESPACE_KEY, auth_name)
                .set(metadata::Identifier::CODE_KEY, code);
        }
        PropertyMap propMethod;
        propMethod.set(common::IdentifiedObject::NAME_KEY,
                       method_name ? method_name : "unnamed");
        if (method_auth_name && method_code) {
            propMethod
                .set(metadata::Identifier::CODESPACE_KEY, method_auth_name)
                .set(metadata::Identifier::CODE_KEY, method_code);
        }
        std::vector<OperationParameterNNPtr> parameters;
        std::vector<ParameterValueNNPtr> values;
        for (int i = 0; i < param_count; i++) {
            PropertyMap propParam;
            propParam.set(common::IdentifiedObject::NAME_KEY,
                          params[i].name ? params[i].name : "unnamed");
            if (params[i].auth_name && params[i].code) {
                propParam
                    .set(metadata::Identifier::CODESPACE_KEY,
                         params[i].auth_name)
                    .set(metadata::Identifier::CODE_KEY, params[i].code);
            }
            parameters.emplace_back(OperationParameter::create(propParam));
            auto unit_type = UnitOfMeasure::Type::UNKNOWN;
            switch (params[i].unit_type) {
            case PJ_UT_ANGULAR:
                unit_type = UnitOfMeasure::Type::ANGULAR;
                break;
            case PJ_UT_LINEAR:
                unit_type = UnitOfMeasure::Type::LINEAR;
                break;
            case PJ_UT_SCALE:
                unit_type = UnitOfMeasure::Type::SCALE;
                break;
            case PJ_UT_TIME:
                unit_type = UnitOfMeasure::Type::TIME;
                break;
            case PJ_UT_PARAMETRIC:
                unit_type = UnitOfMeasure::Type::PARAMETRIC;
                break;
            }

            Measure measure(
                params[i].value,
                params[i].unit_type == PJ_UT_ANGULAR
                    ? createAngularUnit(params[i].unit_name,
                                        params[i].unit_conv_factor)
                    : params[i].unit_type == PJ_UT_LINEAR
                          ? createLinearUnit(params[i].unit_name,
                                             params[i].unit_conv_factor)
                          : UnitOfMeasure(
                                params[i].unit_name ? params[i].unit_name
                                                    : "unnamed",
                                params[i].unit_conv_factor, unit_type));
            values.emplace_back(ParameterValue::create(measure));
        }
        return PJ_OBJ::create(
            Conversion::create(propConv, propMethod, parameters, values));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/**
 * \brief Return an equivalent projection.
 *
 * Currently implemented:
 * <ul>
 * <li>EPSG_CODE_METHOD_MERCATOR_VARIANT_A (1SP) to
 * EPSG_CODE_METHOD_MERCATOR_VARIANT_B (2SP)</li>
 * <li>EPSG_CODE_METHOD_MERCATOR_VARIANT_B (2SP) to
 * EPSG_CODE_METHOD_MERCATOR_VARIANT_A (1SP)</li>
 * <li>EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP to
 * EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP</li>
 * <li>EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP to
 * EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP</li>
 * </ul>
 *
 * @param ctx PROJ context, or NULL for default context
 * @param conversion Object of type Conversion. Must not be NULL.
 * @param new_method_epsg_code EPSG code of the target method. Or 0 (in which
 * case new_method_name must be specified).
 * @param new_method_name EPSG or PROJ target method name. Or nullptr  (in which
 * case new_method_epsg_code must be specified).
 * @return new conversion that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */
PJ_OBJ *proj_obj_convert_conversion_to_other_method(
    PJ_CONTEXT *ctx, const PJ_OBJ *conversion, int new_method_epsg_code,
    const char *new_method_name) {
    SANITIZE_CTX(ctx);
    auto conv = dynamic_cast<const Conversion *>(conversion->obj.get());
    if (!conv) {
        proj_log_error(ctx, __FUNCTION__, "not a Conversion");
        return nullptr;
    }
    if (new_method_epsg_code == 0) {
        if (!new_method_name) {
            return nullptr;
        }
        if (metadata::Identifier::isEquivalentName(
                new_method_name, EPSG_NAME_METHOD_MERCATOR_VARIANT_A)) {
            new_method_epsg_code = EPSG_CODE_METHOD_MERCATOR_VARIANT_A;
        } else if (metadata::Identifier::isEquivalentName(
                       new_method_name, EPSG_NAME_METHOD_MERCATOR_VARIANT_B)) {
            new_method_epsg_code = EPSG_CODE_METHOD_MERCATOR_VARIANT_B;
        } else if (metadata::Identifier::isEquivalentName(
                       new_method_name,
                       EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_1SP)) {
            new_method_epsg_code = EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP;
        } else if (metadata::Identifier::isEquivalentName(
                       new_method_name,
                       EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP)) {
            new_method_epsg_code = EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP;
        }
    }
    try {
        auto new_conv = conv->convertToOtherMethod(new_method_epsg_code);
        if (!new_conv)
            return nullptr;
        return PJ_OBJ::create(NN_NO_CHECK(new_conv));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress

static CoordinateSystemAxisNNPtr createAxis(const PJ_AXIS_DESCRIPTION &axis) {
    const auto dir =
        axis.direction ? AxisDirection::valueOf(axis.direction) : nullptr;
    if (dir == nullptr)
        throw Exception("invalid value for axis direction");
    auto unit_type = UnitOfMeasure::Type::UNKNOWN;
    switch (axis.unit_type) {
    case PJ_UT_ANGULAR:
        unit_type = UnitOfMeasure::Type::ANGULAR;
        break;
    case PJ_UT_LINEAR:
        unit_type = UnitOfMeasure::Type::LINEAR;
        break;
    case PJ_UT_SCALE:
        unit_type = UnitOfMeasure::Type::SCALE;
        break;
    case PJ_UT_TIME:
        unit_type = UnitOfMeasure::Type::TIME;
        break;
    case PJ_UT_PARAMETRIC:
        unit_type = UnitOfMeasure::Type::PARAMETRIC;
        break;
    }
    auto unit =
        axis.unit_type == PJ_UT_ANGULAR
            ? createAngularUnit(axis.unit_name, axis.unit_conv_factor)
            : axis.unit_type == PJ_UT_LINEAR
                  ? createLinearUnit(axis.unit_name, axis.unit_conv_factor)
                  : UnitOfMeasure(axis.unit_name ? axis.unit_name : "unnamed",
                                  axis.unit_conv_factor, unit_type);

    return CoordinateSystemAxis::create(
        createPropertyMapName(axis.name),
        axis.abbreviation ? axis.abbreviation : std::string(), *dir, unit);
}

//! @endcond

// ---------------------------------------------------------------------------

/** \brief Instanciate a CoordinateSystem.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param type Coordinate system type.
 * @param axis_count Number of axis
 * @param axis Axis description (array of size axis_count)
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */

PJ_OBJ *proj_obj_create_cs(PJ_CONTEXT *ctx, PJ_COORDINATE_SYSTEM_TYPE type,
                           int axis_count, const PJ_AXIS_DESCRIPTION *axis) {
    try {
        switch (type) {
        case PJ_CS_TYPE_UNKNOWN:
            return nullptr;

        case PJ_CS_TYPE_CARTESIAN: {
            if (axis_count == 2) {
                return PJ_OBJ::create(CartesianCS::create(
                    PropertyMap(), createAxis(axis[0]), createAxis(axis[1])));
            } else if (axis_count == 3) {
                return PJ_OBJ::create(CartesianCS::create(
                    PropertyMap(), createAxis(axis[0]), createAxis(axis[1]),
                    createAxis(axis[2])));
            }
            break;
        }

        case PJ_CS_TYPE_ELLIPSOIDAL: {
            if (axis_count == 2) {
                return PJ_OBJ::create(EllipsoidalCS::create(
                    PropertyMap(), createAxis(axis[0]), createAxis(axis[1])));
            } else if (axis_count == 3) {
                return PJ_OBJ::create(EllipsoidalCS::create(
                    PropertyMap(), createAxis(axis[0]), createAxis(axis[1]),
                    createAxis(axis[2])));
            }
            break;
        }

        case PJ_CS_TYPE_VERTICAL: {
            if (axis_count == 1) {
                return PJ_OBJ::create(
                    VerticalCS::create(PropertyMap(), createAxis(axis[0])));
            }
            break;
        }

        case PJ_CS_TYPE_SPHERICAL: {
            if (axis_count == 3) {
                return PJ_OBJ::create(EllipsoidalCS::create(
                    PropertyMap(), createAxis(axis[0]), createAxis(axis[1]),
                    createAxis(axis[2])));
            }
            break;
        }

        case PJ_CS_TYPE_PARAMETRIC: {
            if (axis_count == 1) {
                return PJ_OBJ::create(
                    ParametricCS::create(PropertyMap(), createAxis(axis[0])));
            }
            break;
        }

        case PJ_CS_TYPE_ORDINAL: {
            std::vector<CoordinateSystemAxisNNPtr> axisVector;
            for (int i = 0; i < axis_count; i++) {
                axisVector.emplace_back(createAxis(axis[i]));
            }

            return PJ_OBJ::create(OrdinalCS::create(PropertyMap(), axisVector));
        }

        case PJ_CS_TYPE_DATETIMETEMPORAL: {
            if (axis_count == 1) {
                return PJ_OBJ::create(DateTimeTemporalCS::create(
                    PropertyMap(), createAxis(axis[0])));
            }
            break;
        }

        case PJ_CS_TYPE_TEMPORALCOUNT: {
            if (axis_count == 1) {
                return PJ_OBJ::create(TemporalCountCS::create(
                    PropertyMap(), createAxis(axis[0])));
            }
            break;
        }

        case PJ_CS_TYPE_TEMPORALMEASURE: {
            if (axis_count == 1) {
                return PJ_OBJ::create(TemporalMeasureCS::create(
                    PropertyMap(), createAxis(axis[0])));
            }
            break;
        }
        }

    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
    proj_log_error(ctx, __FUNCTION__, "Wrong value for axis_count");
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate a CartesiansCS 2D
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param type Coordinate system type.
 * @param unit_name Unit name.
 * @param unit_conv_factor Unit conversion factor to SI.
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */

PJ_OBJ *proj_obj_create_cartesian_2D_cs(PJ_CONTEXT *ctx,
                                        PJ_CARTESIAN_CS_2D_TYPE type,
                                        const char *unit_name,
                                        double unit_conv_factor) {
    try {
        switch (type) {
        case PJ_CART2D_EASTING_NORTHING:
            return PJ_OBJ::create(CartesianCS::createEastingNorthing(
                createLinearUnit(unit_name, unit_conv_factor)));

        case PJ_CART2D_NORTHING_EASTING:
            return PJ_OBJ::create(CartesianCS::createNorthingEasting(
                createLinearUnit(unit_name, unit_conv_factor)));
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate a Ellipsoidal 2D
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param type Coordinate system type.
 * @param unit_name Unit name.
 * @param unit_conv_factor Unit conversion factor to SI.
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */

PJ_OBJ *proj_obj_create_ellipsoidal_2D_cs(PJ_CONTEXT *ctx,
                                          PJ_ELLIPSOIDAL_CS_2D_TYPE type,
                                          const char *unit_name,
                                          double unit_conv_factor) {
    try {
        switch (type) {
        case PJ_ELLPS2D_LONGITUDE_LATITUDE:
            return PJ_OBJ::create(EllipsoidalCS::createLongitudeLatitude(
                createAngularUnit(unit_name, unit_conv_factor)));

        case PJ_ELLPS2D_LATITUDE_LONGITUDE:
            return PJ_OBJ::create(EllipsoidalCS::createLatitudeLongitude(
                createAngularUnit(unit_name, unit_conv_factor)));
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs_name CRS name. Or NULL
 * @param geodetic_crs Base GeodeticCRS. Must not be NULL.
 * @param conversion Conversion. Must not be NULL.
 * @param coordinate_system Cartesian coordinate system. Must not be NULL.
 *
 * @return Object that must be unreferenced with
 * proj_obj_unref(), or NULL in case of error.
 */

PJ_OBJ *proj_obj_create_projected_crs(PJ_CONTEXT *ctx, const char *crs_name,
                                      const PJ_OBJ *geodetic_crs,
                                      const PJ_OBJ *conversion,
                                      const PJ_OBJ *coordinate_system) {
    SANITIZE_CTX(ctx);
    auto geodCRS =
        util::nn_dynamic_pointer_cast<GeodeticCRS>(geodetic_crs->obj);
    if (!geodCRS) {
        return nullptr;
    }
    auto conv = util::nn_dynamic_pointer_cast<Conversion>(conversion->obj);
    if (!conv) {
        return nullptr;
    }
    auto cs =
        util::nn_dynamic_pointer_cast<CartesianCS>(coordinate_system->obj);
    if (!cs) {
        return nullptr;
    }
    try {
        return PJ_OBJ::create(ProjectedCRS::create(
            createPropertyMapName(crs_name), NN_NO_CHECK(geodCRS),
            NN_NO_CHECK(conv), NN_NO_CHECK(cs)));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

//! @cond Doxygen_Suppress

static PJ_OBJ *proj_obj_create_conversion(const ConversionNNPtr &conv) {
    return PJ_OBJ::create(conv);
}

//! @endcond

/* BEGIN: Generated by scripts/create_c_api_projections.py*/

// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a Universal Transverse Mercator
 * conversion.
 *
 * See osgeo::proj::operation::Conversion::createUTM().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_utm(PJ_CONTEXT *ctx, int zone, int north) {
    SANITIZE_CTX(ctx);
    try {
        auto conv = Conversion::createUTM(PropertyMap(), zone, north != 0);
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Transverse
 * Mercator projection method.
 *
 * See osgeo::proj::operation::Conversion::createTransverseMercator().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_transverse_mercator(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createTransverseMercator(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Gauss
 * Schreiber Transverse Mercator projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createGaussSchreiberTransverseMercator().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_gauss_schreiber_transverse_mercator(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createGaussSchreiberTransverseMercator(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Transverse
 * Mercator South Orientated projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createTransverseMercatorSouthOriented().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_transverse_mercator_south_oriented(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createTransverseMercatorSouthOriented(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Two Point
 * Equidistant projection method.
 *
 * See osgeo::proj::operation::Conversion::createTwoPointEquidistant().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_two_point_equidistant(
    PJ_CONTEXT *ctx, double latitude_first_point, double longitude_first_point,
    double latitude_second_point, double longitude_secon_point,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createTwoPointEquidistant(
            PropertyMap(), Angle(latitude_first_point, angUnit),
            Angle(longitude_first_point, angUnit),
            Angle(latitude_second_point, angUnit),
            Angle(longitude_secon_point, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Tunisia
 * Mapping Grid projection method.
 *
 * See osgeo::proj::operation::Conversion::createTunisiaMappingGrid().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_tunisia_mapping_grid(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createTunisiaMappingGrid(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Albers
 * Conic Equal Area projection method.
 *
 * See osgeo::proj::operation::Conversion::createAlbersEqualArea().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_albers_equal_area(
    PJ_CONTEXT *ctx, double latitude_false_origin,
    double longitude_false_origin, double latitude_first_parallel,
    double latitude_second_parallel, double easting_false_origin,
    double northing_false_origin, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createAlbersEqualArea(
            PropertyMap(), Angle(latitude_false_origin, angUnit),
            Angle(longitude_false_origin, angUnit),
            Angle(latitude_first_parallel, angUnit),
            Angle(latitude_second_parallel, angUnit),
            Length(easting_false_origin, linearUnit),
            Length(northing_false_origin, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Conic Conformal 1SP projection method.
 *
 * See osgeo::proj::operation::Conversion::createLambertConicConformal_1SP().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_conic_conformal_1sp(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertConicConformal_1SP(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Conic Conformal (2SP) projection method.
 *
 * See osgeo::proj::operation::Conversion::createLambertConicConformal_2SP().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_conic_conformal_2sp(
    PJ_CONTEXT *ctx, double latitude_false_origin,
    double longitude_false_origin, double latitude_first_parallel,
    double latitude_second_parallel, double easting_false_origin,
    double northing_false_origin, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertConicConformal_2SP(
            PropertyMap(), Angle(latitude_false_origin, angUnit),
            Angle(longitude_false_origin, angUnit),
            Angle(latitude_first_parallel, angUnit),
            Angle(latitude_second_parallel, angUnit),
            Length(easting_false_origin, linearUnit),
            Length(northing_false_origin, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Conic Conformal (2SP Michigan) projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createLambertConicConformal_2SP_Michigan().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_conic_conformal_2sp_michigan(
    PJ_CONTEXT *ctx, double latitude_false_origin,
    double longitude_false_origin, double latitude_first_parallel,
    double latitude_second_parallel, double easting_false_origin,
    double northing_false_origin, double ellipsoid_scaling_factor,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertConicConformal_2SP_Michigan(
            PropertyMap(), Angle(latitude_false_origin, angUnit),
            Angle(longitude_false_origin, angUnit),
            Angle(latitude_first_parallel, angUnit),
            Angle(latitude_second_parallel, angUnit),
            Length(easting_false_origin, linearUnit),
            Length(northing_false_origin, linearUnit),
            Scale(ellipsoid_scaling_factor));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Conic Conformal (2SP Belgium) projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createLambertConicConformal_2SP_Belgium().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_conic_conformal_2sp_belgium(
    PJ_CONTEXT *ctx, double latitude_false_origin,
    double longitude_false_origin, double latitude_first_parallel,
    double latitude_second_parallel, double easting_false_origin,
    double northing_false_origin, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertConicConformal_2SP_Belgium(
            PropertyMap(), Angle(latitude_false_origin, angUnit),
            Angle(longitude_false_origin, angUnit),
            Angle(latitude_first_parallel, angUnit),
            Angle(latitude_second_parallel, angUnit),
            Length(easting_false_origin, linearUnit),
            Length(northing_false_origin, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Modified
 * Azimuthal Equidistant projection method.
 *
 * See osgeo::proj::operation::Conversion::createAzimuthalEquidistant().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_azimuthal_equidistant(
    PJ_CONTEXT *ctx, double latitude_nat_origin, double longitude_nat_origin,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createAzimuthalEquidistant(
            PropertyMap(), Angle(latitude_nat_origin, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Guam
 * Projection projection method.
 *
 * See osgeo::proj::operation::Conversion::createGuamProjection().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_guam_projection(
    PJ_CONTEXT *ctx, double latitude_nat_origin, double longitude_nat_origin,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createGuamProjection(
            PropertyMap(), Angle(latitude_nat_origin, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Bonne
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createBonne().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_bonne(
    PJ_CONTEXT *ctx, double latitude_nat_origin, double longitude_nat_origin,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createBonne(
            PropertyMap(), Angle(latitude_nat_origin, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Cylindrical Equal Area (Spherical) projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createLambertCylindricalEqualAreaSpherical().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_cylindrical_equal_area_spherical(
    PJ_CONTEXT *ctx, double latitude_first_parallel,
    double longitude_nat_origin, double false_easting, double false_northing,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertCylindricalEqualAreaSpherical(
            PropertyMap(), Angle(latitude_first_parallel, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Cylindrical Equal Area (ellipsoidal form) projection method.
 *
 * See osgeo::proj::operation::Conversion::createLambertCylindricalEqualArea().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_cylindrical_equal_area(
    PJ_CONTEXT *ctx, double latitude_first_parallel,
    double longitude_nat_origin, double false_easting, double false_northing,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertCylindricalEqualArea(
            PropertyMap(), Angle(latitude_first_parallel, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * Cassini-Soldner projection method.
 *
 * See osgeo::proj::operation::Conversion::createCassiniSoldner().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_cassini_soldner(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createCassiniSoldner(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Equidistant
 * Conic projection method.
 *
 * See osgeo::proj::operation::Conversion::createEquidistantConic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_equidistant_conic(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double latitude_first_parallel, double latitude_second_parallel,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEquidistantConic(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit),
            Angle(latitude_first_parallel, angUnit),
            Angle(latitude_second_parallel, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Eckert I
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEckertI().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_eckert_i(PJ_CONTEXT *ctx, double center_long,
                                            double false_easting,
                                            double false_northing,
                                            const char *ang_unit_name,
                                            double ang_unit_conv_factor,
                                            const char *linear_unit_name,
                                            double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEckertI(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Eckert II
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEckertII().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_eckert_ii(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEckertII(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Eckert III
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEckertIII().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_eckert_iii(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEckertIII(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Eckert IV
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEckertIV().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_eckert_iv(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEckertIV(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Eckert V
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEckertV().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_eckert_v(PJ_CONTEXT *ctx, double center_long,
                                            double false_easting,
                                            double false_northing,
                                            const char *ang_unit_name,
                                            double ang_unit_conv_factor,
                                            const char *linear_unit_name,
                                            double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEckertV(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Eckert VI
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEckertVI().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_eckert_vi(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEckertVI(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Equidistant
 * Cylindrical projection method.
 *
 * See osgeo::proj::operation::Conversion::createEquidistantCylindrical().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_equidistant_cylindrical(
    PJ_CONTEXT *ctx, double latitude_first_parallel,
    double longitude_nat_origin, double false_easting, double false_northing,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEquidistantCylindrical(
            PropertyMap(), Angle(latitude_first_parallel, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Equidistant
 * Cylindrical (Spherical) projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createEquidistantCylindricalSpherical().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_equidistant_cylindrical_spherical(
    PJ_CONTEXT *ctx, double latitude_first_parallel,
    double longitude_nat_origin, double false_easting, double false_northing,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEquidistantCylindricalSpherical(
            PropertyMap(), Angle(latitude_first_parallel, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Gall
 * (Stereographic) projection method.
 *
 * See osgeo::proj::operation::Conversion::createGall().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_gall(PJ_CONTEXT *ctx, double center_long,
                                        double false_easting,
                                        double false_northing,
                                        const char *ang_unit_name,
                                        double ang_unit_conv_factor,
                                        const char *linear_unit_name,
                                        double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv =
            Conversion::createGall(PropertyMap(), Angle(center_long, angUnit),
                                   Length(false_easting, linearUnit),
                                   Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Goode
 * Homolosine projection method.
 *
 * See osgeo::proj::operation::Conversion::createGoodeHomolosine().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_goode_homolosine(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createGoodeHomolosine(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Interrupted
 * Goode Homolosine projection method.
 *
 * See osgeo::proj::operation::Conversion::createInterruptedGoodeHomolosine().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_interrupted_goode_homolosine(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createInterruptedGoodeHomolosine(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * Geostationary Satellite View projection method, with the sweep angle axis of
 * the viewing instrument being x.
 *
 * See osgeo::proj::operation::Conversion::createGeostationarySatelliteSweepX().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_geostationary_satellite_sweep_x(
    PJ_CONTEXT *ctx, double center_long, double height, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createGeostationarySatelliteSweepX(
            PropertyMap(), Angle(center_long, angUnit),
            Length(height, linearUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * Geostationary Satellite View projection method, with the sweep angle axis of
 * the viewing instrument being y.
 *
 * See osgeo::proj::operation::Conversion::createGeostationarySatelliteSweepY().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_geostationary_satellite_sweep_y(
    PJ_CONTEXT *ctx, double center_long, double height, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createGeostationarySatelliteSweepY(
            PropertyMap(), Angle(center_long, angUnit),
            Length(height, linearUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Gnomonic
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createGnomonic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_gnomonic(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createGnomonic(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Hotine
 * Oblique Mercator (Variant A) projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createHotineObliqueMercatorVariantA().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_hotine_oblique_mercator_variant_a(
    PJ_CONTEXT *ctx, double latitude_projection_centre,
    double longitude_projection_centre, double azimuth_initial_line,
    double angle_from_rectified_to_skrew_grid, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createHotineObliqueMercatorVariantA(
            PropertyMap(), Angle(latitude_projection_centre, angUnit),
            Angle(longitude_projection_centre, angUnit),
            Angle(azimuth_initial_line, angUnit),
            Angle(angle_from_rectified_to_skrew_grid, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Hotine
 * Oblique Mercator (Variant B) projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createHotineObliqueMercatorVariantB().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_hotine_oblique_mercator_variant_b(
    PJ_CONTEXT *ctx, double latitude_projection_centre,
    double longitude_projection_centre, double azimuth_initial_line,
    double angle_from_rectified_to_skrew_grid, double scale,
    double easting_projection_centre, double northing_projection_centre,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createHotineObliqueMercatorVariantB(
            PropertyMap(), Angle(latitude_projection_centre, angUnit),
            Angle(longitude_projection_centre, angUnit),
            Angle(azimuth_initial_line, angUnit),
            Angle(angle_from_rectified_to_skrew_grid, angUnit), Scale(scale),
            Length(easting_projection_centre, linearUnit),
            Length(northing_projection_centre, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Hotine
 * Oblique Mercator Two Point Natural Origin projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createHotineObliqueMercatorTwoPointNaturalOrigin().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *
proj_obj_create_conversion_hotine_oblique_mercator_two_point_natural_origin(
    PJ_CONTEXT *ctx, double latitude_projection_centre, double latitude_point1,
    double longitude_point1, double latitude_point2, double longitude_point2,
    double scale, double easting_projection_centre,
    double northing_projection_centre, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv =
            Conversion::createHotineObliqueMercatorTwoPointNaturalOrigin(
                PropertyMap(), Angle(latitude_projection_centre, angUnit),
                Angle(latitude_point1, angUnit),
                Angle(longitude_point1, angUnit),
                Angle(latitude_point2, angUnit),
                Angle(longitude_point2, angUnit), Scale(scale),
                Length(easting_projection_centre, linearUnit),
                Length(northing_projection_centre, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Laborde
 * Oblique Mercator projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createLabordeObliqueMercator().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_laborde_oblique_mercator(
    PJ_CONTEXT *ctx, double latitude_projection_centre,
    double longitude_projection_centre, double azimuth_initial_line,
    double scale, double false_easting, double false_northing,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLabordeObliqueMercator(
            PropertyMap(), Angle(latitude_projection_centre, angUnit),
            Angle(longitude_projection_centre, angUnit),
            Angle(azimuth_initial_line, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * International Map of the World Polyconic projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createInternationalMapWorldPolyconic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_international_map_world_polyconic(
    PJ_CONTEXT *ctx, double center_long, double latitude_first_parallel,
    double latitude_second_parallel, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createInternationalMapWorldPolyconic(
            PropertyMap(), Angle(center_long, angUnit),
            Angle(latitude_first_parallel, angUnit),
            Angle(latitude_second_parallel, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Krovak
 * (north oriented) projection method.
 *
 * See osgeo::proj::operation::Conversion::createKrovakNorthOriented().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_krovak_north_oriented(
    PJ_CONTEXT *ctx, double latitude_projection_centre,
    double longitude_of_origin, double colatitude_cone_axis,
    double latitude_pseudo_standard_parallel,
    double scale_factor_pseudo_standard_parallel, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createKrovakNorthOriented(
            PropertyMap(), Angle(latitude_projection_centre, angUnit),
            Angle(longitude_of_origin, angUnit),
            Angle(colatitude_cone_axis, angUnit),
            Angle(latitude_pseudo_standard_parallel, angUnit),
            Scale(scale_factor_pseudo_standard_parallel),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Krovak
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createKrovak().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_krovak(
    PJ_CONTEXT *ctx, double latitude_projection_centre,
    double longitude_of_origin, double colatitude_cone_axis,
    double latitude_pseudo_standard_parallel,
    double scale_factor_pseudo_standard_parallel, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createKrovak(
            PropertyMap(), Angle(latitude_projection_centre, angUnit),
            Angle(longitude_of_origin, angUnit),
            Angle(colatitude_cone_axis, angUnit),
            Angle(latitude_pseudo_standard_parallel, angUnit),
            Scale(scale_factor_pseudo_standard_parallel),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Lambert
 * Azimuthal Equal Area projection method.
 *
 * See osgeo::proj::operation::Conversion::createLambertAzimuthalEqualArea().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_lambert_azimuthal_equal_area(
    PJ_CONTEXT *ctx, double latitude_nat_origin, double longitude_nat_origin,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createLambertAzimuthalEqualArea(
            PropertyMap(), Angle(latitude_nat_origin, angUnit),
            Angle(longitude_nat_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Miller
 * Cylindrical projection method.
 *
 * See osgeo::proj::operation::Conversion::createMillerCylindrical().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_miller_cylindrical(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createMillerCylindrical(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Mercator
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createMercatorVariantA().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_mercator_variant_a(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createMercatorVariantA(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Mercator
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createMercatorVariantB().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_mercator_variant_b(
    PJ_CONTEXT *ctx, double latitude_first_parallel, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createMercatorVariantB(
            PropertyMap(), Angle(latitude_first_parallel, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Popular
 * Visualisation Pseudo Mercator projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createPopularVisualisationPseudoMercator().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_popular_visualisation_pseudo_mercator(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createPopularVisualisationPseudoMercator(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Mollweide
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createMollweide().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_mollweide(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createMollweide(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the New Zealand
 * Map Grid projection method.
 *
 * See osgeo::proj::operation::Conversion::createNewZealandMappingGrid().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_new_zealand_mapping_grid(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createNewZealandMappingGrid(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Oblique
 * Stereographic (Alternative) projection method.
 *
 * See osgeo::proj::operation::Conversion::createObliqueStereographic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_oblique_stereographic(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createObliqueStereographic(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * Orthographic projection method.
 *
 * See osgeo::proj::operation::Conversion::createOrthographic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_orthographic(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createOrthographic(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the American
 * Polyconic projection method.
 *
 * See osgeo::proj::operation::Conversion::createAmericanPolyconic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_american_polyconic(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createAmericanPolyconic(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Polar
 * Stereographic (Variant A) projection method.
 *
 * See osgeo::proj::operation::Conversion::createPolarStereographicVariantA().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_polar_stereographic_variant_a(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createPolarStereographicVariantA(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Polar
 * Stereographic (Variant B) projection method.
 *
 * See osgeo::proj::operation::Conversion::createPolarStereographicVariantB().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_polar_stereographic_variant_b(
    PJ_CONTEXT *ctx, double latitude_standard_parallel,
    double longitude_of_origin, double false_easting, double false_northing,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createPolarStereographicVariantB(
            PropertyMap(), Angle(latitude_standard_parallel, angUnit),
            Angle(longitude_of_origin, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Robinson
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createRobinson().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_robinson(PJ_CONTEXT *ctx, double center_long,
                                            double false_easting,
                                            double false_northing,
                                            const char *ang_unit_name,
                                            double ang_unit_conv_factor,
                                            const char *linear_unit_name,
                                            double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createRobinson(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Sinusoidal
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createSinusoidal().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_sinusoidal(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createSinusoidal(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * Stereographic projection method.
 *
 * See osgeo::proj::operation::Conversion::createStereographic().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_stereographic(
    PJ_CONTEXT *ctx, double center_lat, double center_long, double scale,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createStereographic(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Scale(scale),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Van der
 * Grinten projection method.
 *
 * See osgeo::proj::operation::Conversion::createVanDerGrinten().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_van_der_grinten(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createVanDerGrinten(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner I
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerI().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_i(PJ_CONTEXT *ctx, double center_long,
                                            double false_easting,
                                            double false_northing,
                                            const char *ang_unit_name,
                                            double ang_unit_conv_factor,
                                            const char *linear_unit_name,
                                            double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerI(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner II
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerII().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_ii(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerII(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner III
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerIII().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_iii(
    PJ_CONTEXT *ctx, double latitude_true_scale, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerIII(
            PropertyMap(), Angle(latitude_true_scale, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner IV
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerIV().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_iv(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerIV(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner V
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerV().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_v(PJ_CONTEXT *ctx, double center_long,
                                            double false_easting,
                                            double false_northing,
                                            const char *ang_unit_name,
                                            double ang_unit_conv_factor,
                                            const char *linear_unit_name,
                                            double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerV(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner VI
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerVI().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_vi(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerVI(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Wagner VII
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createWagnerVII().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_wagner_vii(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createWagnerVII(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the
 * Quadrilateralized Spherical Cube projection method.
 *
 * See
 * osgeo::proj::operation::Conversion::createQuadrilateralizedSphericalCube().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_quadrilateralized_spherical_cube(
    PJ_CONTEXT *ctx, double center_lat, double center_long,
    double false_easting, double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createQuadrilateralizedSphericalCube(
            PropertyMap(), Angle(center_lat, angUnit),
            Angle(center_long, angUnit), Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Spherical
 * Cross-Track Height projection method.
 *
 * See osgeo::proj::operation::Conversion::createSphericalCrossTrackHeight().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_spherical_cross_track_height(
    PJ_CONTEXT *ctx, double peg_point_lat, double peg_point_long,
    double peg_point_heading, double peg_point_height,
    const char *ang_unit_name, double ang_unit_conv_factor,
    const char *linear_unit_name, double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createSphericalCrossTrackHeight(
            PropertyMap(), Angle(peg_point_lat, angUnit),
            Angle(peg_point_long, angUnit), Angle(peg_point_heading, angUnit),
            Length(peg_point_height, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
// ---------------------------------------------------------------------------

/** \brief Instanciate a ProjectedCRS with a conversion based on the Equal Earth
 * projection method.
 *
 * See osgeo::proj::operation::Conversion::createEqualEarth().
 *
 * Linear parameters are expressed in (linear_unit_name,
 * linear_unit_conv_factor).
 * Angular parameters are expressed in (ang_unit_name, ang_unit_conv_factor).
 */
PJ_OBJ *proj_obj_create_conversion_equal_earth(
    PJ_CONTEXT *ctx, double center_long, double false_easting,
    double false_northing, const char *ang_unit_name,
    double ang_unit_conv_factor, const char *linear_unit_name,
    double linear_unit_conv_factor) {
    SANITIZE_CTX(ctx);
    try {
        UnitOfMeasure linearUnit(
            createLinearUnit(linear_unit_name, linear_unit_conv_factor));
        UnitOfMeasure angUnit(
            createAngularUnit(ang_unit_name, ang_unit_conv_factor));
        auto conv = Conversion::createEqualEarth(
            PropertyMap(), Angle(center_long, angUnit),
            Length(false_easting, linearUnit),
            Length(false_northing, linearUnit));
        return proj_obj_create_conversion(conv);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}
/* END: Generated by scripts/create_c_api_projections.py*/

// ---------------------------------------------------------------------------

/** \brief Return whether a coordinate operation can be instanciated as
 * a PROJ pipeline, checking in particular that referenced grids are
 * available.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type CoordinateOperation or derived classes
 * (must not be NULL)
 * @return TRUE or FALSE.
 */

int proj_coordoperation_is_instanciable(PJ_CONTEXT *ctx,
                                        const PJ_OBJ *coordoperation) {
    assert(coordoperation);
    auto op =
        dynamic_cast<const CoordinateOperation *>(coordoperation->obj.get());
    if (!op) {
        proj_log_error(ctx, __FUNCTION__,
                       "Object is not a CoordinateOperation");
        return 0;
    }
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    try {
        return op->isPROJInstanciable(dbContext) ? 1 : 0;
    } catch (const std::exception &) {
        return 0;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return the number of parameters of a SingleOperation
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type SingleOperation or derived classes
 * (must not be NULL)
 */

int proj_coordoperation_get_param_count(PJ_CONTEXT *ctx,
                                        const PJ_OBJ *coordoperation) {
    SANITIZE_CTX(ctx);
    assert(coordoperation);
    auto op = dynamic_cast<const SingleOperation *>(coordoperation->obj.get());
    if (!op) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a SingleOperation");
        return 0;
    }
    return static_cast<int>(op->parameterValues().size());
}

// ---------------------------------------------------------------------------

/** \brief Return the index of a parameter of a SingleOperation
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type SingleOperation or derived classes
 * (must not be NULL)
 * @param name Parameter name. Must not be NULL
 * @return index (>=0), or -1 in case of error.
 */

int proj_coordoperation_get_param_index(PJ_CONTEXT *ctx,
                                        const PJ_OBJ *coordoperation,
                                        const char *name) {
    SANITIZE_CTX(ctx);
    assert(coordoperation);
    assert(name);
    auto op = dynamic_cast<const SingleOperation *>(coordoperation->obj.get());
    if (!op) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a SingleOperation");
        return -1;
    }
    int index = 0;
    for (const auto &genParam : op->method()->parameters()) {
        if (Identifier::isEquivalentName(genParam->nameStr().c_str(), name)) {
            return index;
        }
        index++;
    }
    return -1;
}

// ---------------------------------------------------------------------------

/** \brief Return a parameter of a SingleOperation
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type SingleOperation or derived classes
 * (must not be NULL)
 * @param index Parameter index.
 * @param out_name Pointer to a string value to store the parameter name. or
 * NULL
 * @param out_auth_name Pointer to a string value to store the parameter
 * authority name. or NULL
 * @param out_code Pointer to a string value to store the parameter
 * code. or NULL
 * @param out_value Pointer to a double value to store the parameter
 * value (if numeric). or NULL
 * @param out_value_string Pointer to a string value to store the parameter
 * value (if of type string). or NULL
 * @param out_unit_conv_factor Pointer to a double value to store the parameter
 * unit conversion factor. or NULL
 * @param out_unit_name Pointer to a string value to store the parameter
 * unit name. or NULL
 * @param out_unit_auth_name Pointer to a string value to store the
 * unit authority name. or NULL
 * @param out_unit_code Pointer to a string value to store the
 * unit code. or NULL
 * @param out_unit_category Pointer to a string value to store the parameter
 * name. or
 * NULL. This value might be "unknown", "none", "linear", "angular", "scale",
 * "time" or "parametric";
 * @return TRUE in case of success.
 */

int proj_coordoperation_get_param(
    PJ_CONTEXT *ctx, const PJ_OBJ *coordoperation, int index,
    const char **out_name, const char **out_auth_name, const char **out_code,
    double *out_value, const char **out_value_string,
    double *out_unit_conv_factor, const char **out_unit_name,
    const char **out_unit_auth_name, const char **out_unit_code,
    const char **out_unit_category) {
    SANITIZE_CTX(ctx);
    assert(coordoperation);
    auto op = dynamic_cast<const SingleOperation *>(coordoperation->obj.get());
    if (!op) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a SingleOperation");
        return false;
    }
    const auto &parameters = op->method()->parameters();
    const auto &values = op->parameterValues();
    if (static_cast<size_t>(index) >= parameters.size() ||
        static_cast<size_t>(index) >= values.size()) {
        proj_log_error(ctx, __FUNCTION__, "Invalid index");
        return false;
    }

    const auto &param = parameters[index];
    const auto &param_ids = param->identifiers();
    if (out_name) {
        *out_name = param->name()->description()->c_str();
    }
    if (out_auth_name) {
        if (!param_ids.empty()) {
            *out_auth_name = param_ids[0]->codeSpace()->c_str();
        } else {
            *out_auth_name = nullptr;
        }
    }
    if (out_code) {
        if (!param_ids.empty()) {
            *out_code = param_ids[0]->code().c_str();
        } else {
            *out_code = nullptr;
        }
    }

    const auto &value = values[index];
    ParameterValuePtr paramValue = nullptr;
    auto opParamValue =
        dynamic_cast<const OperationParameterValue *>(value.get());
    if (opParamValue) {
        paramValue = opParamValue->parameterValue().as_nullable();
    }
    if (out_value) {
        *out_value = 0;
        if (paramValue) {
            if (paramValue->type() == ParameterValue::Type::MEASURE) {
                *out_value = paramValue->value().value();
            }
        }
    }
    if (out_value_string) {
        *out_value_string = nullptr;
        if (paramValue) {
            if (paramValue->type() == ParameterValue::Type::FILENAME) {
                *out_value_string = paramValue->valueFile().c_str();
            } else if (paramValue->type() == ParameterValue::Type::STRING) {
                *out_value_string = paramValue->stringValue().c_str();
            }
        }
    }
    if (out_unit_conv_factor) {
        *out_unit_conv_factor = 0;
    }
    if (out_unit_name) {
        *out_unit_name = nullptr;
    }
    if (out_unit_auth_name) {
        *out_unit_auth_name = nullptr;
    }
    if (out_unit_code) {
        *out_unit_code = nullptr;
    }
    if (out_unit_category) {
        *out_unit_category = nullptr;
    }
    if (paramValue) {
        if (paramValue->type() == ParameterValue::Type::MEASURE) {
            const auto &unit = paramValue->value().unit();
            if (out_unit_conv_factor) {
                *out_unit_conv_factor = unit.conversionToSI();
            }
            if (out_unit_name) {
                *out_unit_name = unit.name().c_str();
            }
            if (out_unit_auth_name) {
                *out_unit_auth_name = unit.codeSpace().c_str();
            }
            if (out_unit_code) {
                *out_unit_code = unit.code().c_str();
            }
            if (out_unit_category) {
                *out_unit_category = get_unit_category(unit.type());
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------

/** \brief Return the number of grids used by a CoordinateOperation
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type CoordinateOperation or derived classes
 * (must not be NULL)
 */

int proj_coordoperation_get_grid_used_count(PJ_CONTEXT *ctx,
                                            const PJ_OBJ *coordoperation) {
    SANITIZE_CTX(ctx);
    assert(coordoperation);
    auto co =
        dynamic_cast<const CoordinateOperation *>(coordoperation->obj.get());
    if (!co) {
        proj_log_error(ctx, __FUNCTION__,
                       "Object is not a CoordinateOperation");
        return 0;
    }
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    try {
        if (!coordoperation->gridsNeededAsked) {
            coordoperation->gridsNeededAsked = true;
            const auto gridsNeeded = co->gridsNeeded(dbContext);
            for (const auto &gridDesc : gridsNeeded) {
                coordoperation->gridsNeeded.emplace_back(gridDesc);
            }
        }
        return static_cast<int>(coordoperation->gridsNeeded.size());
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return 0;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return a parameter of a SingleOperation
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Objet of type SingleOperation or derived classes
 * (must not be NULL)
 * @param index Parameter index.
 * @param out_short_name Pointer to a string value to store the grid short name.
 * or NULL
 * @param out_full_name Pointer to a string value to store the grid full
 * filename. or NULL
 * @param out_package_name Pointer to a string value to store the package name
 * where
 * the grid might be found. or NULL
 * @param out_url Pointer to a string value to store the grid URL or the
 * package URL where the grid might be found. or NULL
 * @param out_direct_download Pointer to a int (boolean) value to store whether
 * *out_url can be downloaded directly. or NULL
 * @param out_open_license Pointer to a int (boolean) value to store whether
 * the grid is released with an open license. or NULL
 * @param out_available Pointer to a int (boolean) value to store whether the
 * grid is available at runtime. or NULL
 * @return TRUE in case of success.
 */

int proj_coordoperation_get_grid_used(
    PJ_CONTEXT *ctx, const PJ_OBJ *coordoperation, int index,
    const char **out_short_name, const char **out_full_name,
    const char **out_package_name, const char **out_url,
    int *out_direct_download, int *out_open_license, int *out_available) {
    SANITIZE_CTX(ctx);
    const int count =
        proj_coordoperation_get_grid_used_count(ctx, coordoperation);
    if (index < 0 || index >= count) {
        proj_log_error(ctx, __FUNCTION__, "Invalid index");
        return false;
    }

    const auto &gridDesc = coordoperation->gridsNeeded[index];
    if (out_short_name) {
        *out_short_name = gridDesc.shortName.c_str();
    }

    if (out_full_name) {
        *out_full_name = gridDesc.fullName.c_str();
    }

    if (out_package_name) {
        *out_package_name = gridDesc.packageName.c_str();
    }

    if (out_url) {
        *out_url = gridDesc.url.c_str();
    }

    if (out_direct_download) {
        *out_direct_download = gridDesc.directDownload;
    }

    if (out_open_license) {
        *out_open_license = gridDesc.openLicense;
    }

    if (out_available) {
        *out_available = gridDesc.available;
    }

    return true;
}

// ---------------------------------------------------------------------------

/** \brief Opaque object representing an operation factory context. */
struct PJ_OPERATION_FACTORY_CONTEXT {
    //! @cond Doxygen_Suppress
    CoordinateOperationContextNNPtr operationContext;

    explicit PJ_OPERATION_FACTORY_CONTEXT(
        CoordinateOperationContextNNPtr &&operationContextIn)
        : operationContext(std::move(operationContextIn)) {}

    PJ_OPERATION_FACTORY_CONTEXT(const PJ_OPERATION_FACTORY_CONTEXT &) = delete;
    PJ_OPERATION_FACTORY_CONTEXT &
    operator=(const PJ_OPERATION_FACTORY_CONTEXT &) = delete;
    //! @endcond
};

// ---------------------------------------------------------------------------

/** \brief Instanciate a context for building coordinate operations between
 * two CRS.
 *
 * The returned object must be unreferenced with
 * proj_operation_factory_context_unref() after use.
 *
 * If authority is NULL or the empty string, then coordinate
 * operations from any authority will be searched, with the restrictions set
 * in the authority_to_authority_preference database table.
 * If authority is set to "any", then coordinate
 * operations from any authority will be searched
 * If authority is a non-empty string different of "any",
 * then coordinate operatiosn will be searched only in that authority namespace.
 *
 * @param ctx Context, or NULL for default context.
 * @param authority Name of authority to which to restrict the search of
 *                  candidate operations.
 * @return Object that must be unreferenced with
 * proj_operation_factory_context_unref(), or NULL in
 * case of error.
 */
PJ_OPERATION_FACTORY_CONTEXT *
proj_create_operation_factory_context(PJ_CONTEXT *ctx, const char *authority) {
    SANITIZE_CTX(ctx);
    auto dbContext = getDBcontextNoException(ctx, __FUNCTION__);
    try {
        if (dbContext) {
            auto factory = CoordinateOperationFactory::create();
            auto authFactory = AuthorityFactory::create(
                NN_NO_CHECK(dbContext),
                std::string(authority ? authority : ""));
            auto operationContext =
                CoordinateOperationContext::create(authFactory, nullptr, 0.0);
            return new PJ_OPERATION_FACTORY_CONTEXT(
                std::move(operationContext));
        } else {
            auto operationContext =
                CoordinateOperationContext::create(nullptr, nullptr, 0.0);
            return new PJ_OPERATION_FACTORY_CONTEXT(
                std::move(operationContext));
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

/** \brief Drops a reference on an object.
 *
 * This method should be called one and exactly one for each function
 * returning a PJ_OPERATION_FACTORY_CONTEXT*
 *
 * @param ctx Object, or NULL.
 */
void proj_operation_factory_context_unref(PJ_OPERATION_FACTORY_CONTEXT *ctx) {
    delete ctx;
}

// ---------------------------------------------------------------------------

/** \brief Set the desired accuracy of the resulting coordinate transformations.
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param accuracy Accuracy in meter (or 0 to disable the filter).
 */
void proj_operation_factory_context_set_desired_accuracy(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    double accuracy) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        factory_ctx->operationContext->setDesiredAccuracy(accuracy);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Set the desired area of interest for the resulting coordinate
 * transformations.
 *
 * For an area of interest crossing the anti-meridian, west_lon_degree will be
 * greater than east_lon_degree.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param west_lon_degree West longitude (in degrees).
 * @param south_lat_degree South latitude (in degrees).
 * @param east_lon_degree East longitude (in degrees).
 * @param north_lat_degree North latitude (in degrees).
 */
void proj_operation_factory_context_set_area_of_interest(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    double west_lon_degree, double south_lat_degree, double east_lon_degree,
    double north_lat_degree) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        factory_ctx->operationContext->setAreaOfInterest(
            Extent::createFromBBOX(west_lon_degree, south_lat_degree,
                                   east_lon_degree, north_lat_degree));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Set how source and target CRS extent should be used
 * when considering if a transformation can be used (only takes effect if
 * no area of interest is explicitly defined).
 *
 * The default is PJ_CRS_EXTENT_SMALLEST.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param use How source and target CRS extent should be used.
 */
void proj_operation_factory_context_set_crs_extent_use(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    PROJ_CRS_EXTENT_USE use) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        switch (use) {
        case PJ_CRS_EXTENT_NONE:
            factory_ctx->operationContext->setSourceAndTargetCRSExtentUse(
                CoordinateOperationContext::SourceTargetCRSExtentUse::NONE);
            break;

        case PJ_CRS_EXTENT_BOTH:
            factory_ctx->operationContext->setSourceAndTargetCRSExtentUse(
                CoordinateOperationContext::SourceTargetCRSExtentUse::BOTH);
            break;

        case PJ_CRS_EXTENT_INTERSECTION:
            factory_ctx->operationContext->setSourceAndTargetCRSExtentUse(
                CoordinateOperationContext::SourceTargetCRSExtentUse::
                    INTERSECTION);
            break;

        case PJ_CRS_EXTENT_SMALLEST:
            factory_ctx->operationContext->setSourceAndTargetCRSExtentUse(
                CoordinateOperationContext::SourceTargetCRSExtentUse::SMALLEST);
            break;
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Set the spatial criterion to use when comparing the area of
 * validity of coordinate operations with the area of interest / area of
 * validity of
 * source and target CRS.
 *
 * The default is PROJ_SPATIAL_CRITERION_STRICT_CONTAINMENT.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param criterion patial criterion to use
 */
void PROJ_DLL proj_operation_factory_context_set_spatial_criterion(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    PROJ_SPATIAL_CRITERION criterion) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        switch (criterion) {
        case PROJ_SPATIAL_CRITERION_STRICT_CONTAINMENT:
            factory_ctx->operationContext->setSpatialCriterion(
                CoordinateOperationContext::SpatialCriterion::
                    STRICT_CONTAINMENT);
            break;

        case PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION:
            factory_ctx->operationContext->setSpatialCriterion(
                CoordinateOperationContext::SpatialCriterion::
                    PARTIAL_INTERSECTION);
            break;
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Set how grid availability is used.
 *
 * The default is USE_FOR_SORTING.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param use how grid availability is used.
 */
void PROJ_DLL proj_operation_factory_context_set_grid_availability_use(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    PROJ_GRID_AVAILABILITY_USE use) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        switch (use) {
        case PROJ_GRID_AVAILABILITY_USED_FOR_SORTING:
            factory_ctx->operationContext->setGridAvailabilityUse(
                CoordinateOperationContext::GridAvailabilityUse::
                    USE_FOR_SORTING);
            break;

        case PROJ_GRID_AVAILABILITY_DISCARD_OPERATION_IF_MISSING_GRID:
            factory_ctx->operationContext->setGridAvailabilityUse(
                CoordinateOperationContext::GridAvailabilityUse::
                    DISCARD_OPERATION_IF_MISSING_GRID);
            break;

        case PROJ_GRID_AVAILABILITY_IGNORED:
            factory_ctx->operationContext->setGridAvailabilityUse(
                CoordinateOperationContext::GridAvailabilityUse::
                    IGNORE_GRID_AVAILABILITY);
            break;
        }
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Set whether PROJ alternative grid names should be substituted to
 * the official authority names.
 *
 * The default is true.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param usePROJNames whether PROJ alternative grid names should be used
 */
void proj_operation_factory_context_set_use_proj_alternative_grid_names(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    int usePROJNames) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        factory_ctx->operationContext->setUsePROJAlternativeGridNames(
            usePROJNames != 0);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Set whether an intermediate pivot CRS can be used for researching
 * coordinate operations between a source and target CRS.
 *
 * Concretely if in the database there is an operation from A to C
 * (or C to A), and another one from C to B (or B to C), but no direct
 * operation between A and B, setting this parameter to true, allow
 * chaining both operations.
 *
 * The current implementation is limited to researching one intermediate
 * step.
 *
 * By default, all potential C candidates will be used.
 * proj_operation_factory_context_set_allowed_intermediate_crs()
 * can be used to restrict them.
 *
 * The default is true.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param allow whether intermediate CRS may be used.
 */
void proj_operation_factory_context_set_allow_use_intermediate_crs(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx, int allow) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        factory_ctx->operationContext->setAllowUseIntermediateCRS(allow != 0);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Restrict the potential pivot CRSs that can be used when trying to
 * build a coordinate operation between two CRS that have no direct operation.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param factory_ctx Operation factory context. must not be NULL
 * @param list_of_auth_name_codes an array of strings NLL terminated,
 * with the format { "auth_name1", "code1", "auth_name2", "code2", ... NULL }
 */
void proj_operation_factory_context_set_allowed_intermediate_crs(
    PJ_CONTEXT *ctx, PJ_OPERATION_FACTORY_CONTEXT *factory_ctx,
    const char *const *list_of_auth_name_codes) {
    SANITIZE_CTX(ctx);
    assert(factory_ctx);
    try {
        std::vector<std::pair<std::string, std::string>> pivots;
        for (auto iter = list_of_auth_name_codes; iter && iter[0] && iter[1];
             iter += 2) {
            pivots.emplace_back(std::pair<std::string, std::string>(
                std::string(iter[0]), std::string(iter[1])));
        }
        factory_ctx->operationContext->setIntermediateCRS(pivots);
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
    }
}

// ---------------------------------------------------------------------------

/** \brief Find a list of CoordinateOperation from source_crs to target_crs.
 *
 * The operations are sorted with the most relevant ones first: by
 * descending
 * area (intersection of the transformation area with the area of interest,
 * or intersection of the transformation with the area of use of the CRS),
 * and
 * by increasing accuracy. Operations with unknown accuracy are sorted last,
 * whatever their area.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param source_crs source CRS. Must not be NULL.
 * @param target_crs source CRS. Must not be NULL.
 * @param operationContext Search context. Must not be NULL.
 * @return a result set that must be unreferenced with
 * proj_obj_list_unref(), or NULL in case of error.
 */
PJ_OBJ_LIST *proj_obj_create_operations(
    PJ_CONTEXT *ctx, const PJ_OBJ *source_crs, const PJ_OBJ *target_crs,
    const PJ_OPERATION_FACTORY_CONTEXT *operationContext) {
    SANITIZE_CTX(ctx);
    assert(source_crs);
    assert(target_crs);
    assert(operationContext);

    auto sourceCRS = nn_dynamic_pointer_cast<CRS>(source_crs->obj);
    if (!sourceCRS) {
        proj_log_error(ctx, __FUNCTION__, "source_crs is not a CRS");
        return nullptr;
    }
    auto targetCRS = nn_dynamic_pointer_cast<CRS>(target_crs->obj);
    if (!targetCRS) {
        proj_log_error(ctx, __FUNCTION__, "target_crs is not a CRS");
        return nullptr;
    }

    try {
        auto factory = CoordinateOperationFactory::create();
        std::vector<IdentifiedObjectNNPtr> objects;
        auto ops = factory->createOperations(
            NN_NO_CHECK(sourceCRS), NN_NO_CHECK(targetCRS),
            operationContext->operationContext);
        for (const auto &op : ops) {
            objects.emplace_back(op);
        }
        return new PJ_OBJ_LIST(std::move(objects));
    } catch (const std::exception &e) {
        proj_log_error(ctx, __FUNCTION__, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

/** \brief Return the number of objects in the result set
 *
 * @param result Objet of type PJ_OBJ_LIST (must not be NULL)
 */
int proj_obj_list_get_count(const PJ_OBJ_LIST *result) {
    assert(result);
    return static_cast<int>(result->objects.size());
}

// ---------------------------------------------------------------------------

/** \brief Return an object from the result set
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param result Objet of type PJ_OBJ_LIST (must not be NULL)
 * @param index Index
 * @return a new object that must be unreferenced with proj_obj_unref(),
 * or nullptr in case of error.
 */

PJ_OBJ *proj_obj_list_get(PJ_CONTEXT *ctx, const PJ_OBJ_LIST *result,
                          int index) {
    SANITIZE_CTX(ctx);
    assert(result);
    if (index < 0 || index >= proj_obj_list_get_count(result)) {
        proj_log_error(ctx, __FUNCTION__, "Invalid index");
        return nullptr;
    }
    return PJ_OBJ::create(result->objects[index]);
}

// ---------------------------------------------------------------------------

/** \brief Drops a reference on the result set.
 *
 * This method should be called one and exactly one for each function
 * returning a PJ_OBJ_LIST*
 *
 * @param result Object, or NULL.
 */
void proj_obj_list_unref(PJ_OBJ_LIST *result) { delete result; }

// ---------------------------------------------------------------------------

/** \brief Return the accuracy (in metre) of a coordinate operation.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param coordoperation Coordinate operation. Must not be NULL.
 * @return the accuracy, or a negative value if unknown or in case of error.
 */
double proj_coordoperation_get_accuracy(PJ_CONTEXT *ctx,
                                        const PJ_OBJ *coordoperation) {
    SANITIZE_CTX(ctx);
    assert(coordoperation);
    auto co =
        dynamic_cast<const CoordinateOperation *>(coordoperation->obj.get());
    if (!co) {
        proj_log_error(ctx, __FUNCTION__,
                       "Object is not a CoordinateOperation");
        return -1;
    }
    const auto &accuracies = co->coordinateOperationAccuracies();
    if (accuracies.empty()) {
        return -1;
    }
    try {
        return c_locale_stod(accuracies[0]->value());
    } catch (const std::exception &) {
    }
    return -1;
}

// ---------------------------------------------------------------------------

/** \brief Returns the datum of a SingleCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type SingleCRS (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error (or if there is no datum)
 */
PJ_OBJ *proj_obj_crs_get_datum(PJ_CONTEXT *ctx, const PJ_OBJ *crs) {
    SANITIZE_CTX(ctx);
    assert(crs);
    auto l_crs = dynamic_cast<const SingleCRS *>(crs->obj.get());
    if (!l_crs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a SingleCRS");
        return nullptr;
    }
    const auto &datum = l_crs->datum();
    if (!datum) {
        return nullptr;
    }
    return PJ_OBJ::create(NN_NO_CHECK(datum));
}

// ---------------------------------------------------------------------------

/** \brief Returns the coordinate system of a SingleCRS.
 *
 * The returned object must be unreferenced with proj_obj_unref() after
 * use.
 * It should be used by at most one thread at a time.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param crs Objet of type SingleCRS (must not be NULL)
 * @return Object that must be unreferenced with proj_obj_unref(), or NULL
 * in case of error.
 */
PJ_OBJ *proj_obj_crs_get_coordinate_system(PJ_CONTEXT *ctx, const PJ_OBJ *crs) {
    SANITIZE_CTX(ctx);
    assert(crs);
    auto l_crs = dynamic_cast<const SingleCRS *>(crs->obj.get());
    if (!l_crs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a SingleCRS");
        return nullptr;
    }
    return PJ_OBJ::create(l_crs->coordinateSystem());
}

// ---------------------------------------------------------------------------

/** \brief Returns the type of the coordinate system.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param cs Objet of type CoordinateSystem (must not be NULL)
 * @return type, or PJ_CS_TYPE_UNKNOWN in case of error.
 */
PJ_COORDINATE_SYSTEM_TYPE proj_obj_cs_get_type(PJ_CONTEXT *ctx,
                                               const PJ_OBJ *cs) {
    SANITIZE_CTX(ctx);
    assert(cs);
    auto l_cs = dynamic_cast<const CoordinateSystem *>(cs->obj.get());
    if (!l_cs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a CoordinateSystem");
        return PJ_CS_TYPE_UNKNOWN;
    }
    if (dynamic_cast<const CartesianCS *>(l_cs)) {
        return PJ_CS_TYPE_CARTESIAN;
    }
    if (dynamic_cast<const EllipsoidalCS *>(l_cs)) {
        return PJ_CS_TYPE_ELLIPSOIDAL;
    }
    if (dynamic_cast<const VerticalCS *>(l_cs)) {
        return PJ_CS_TYPE_VERTICAL;
    }
    if (dynamic_cast<const SphericalCS *>(l_cs)) {
        return PJ_CS_TYPE_SPHERICAL;
    }
    if (dynamic_cast<const OrdinalCS *>(l_cs)) {
        return PJ_CS_TYPE_ORDINAL;
    }
    if (dynamic_cast<const ParametricCS *>(l_cs)) {
        return PJ_CS_TYPE_PARAMETRIC;
    }
    if (dynamic_cast<const DateTimeTemporalCS *>(l_cs)) {
        return PJ_CS_TYPE_DATETIMETEMPORAL;
    }
    if (dynamic_cast<const TemporalCountCS *>(l_cs)) {
        return PJ_CS_TYPE_TEMPORALCOUNT;
    }
    if (dynamic_cast<const TemporalMeasureCS *>(l_cs)) {
        return PJ_CS_TYPE_TEMPORALMEASURE;
    }
    return PJ_CS_TYPE_UNKNOWN;
}

// ---------------------------------------------------------------------------

/** \brief Returns the number of axis of the coordinate system.
 *
 * @param ctx PROJ context, or NULL for default context
 * @param cs Objet of type CoordinateSystem (must not be NULL)
 * @return number of axis, or -1 in case of error.
 */
int proj_obj_cs_get_axis_count(PJ_CONTEXT *ctx, const PJ_OBJ *cs) {
    SANITIZE_CTX(ctx);
    assert(cs);
    auto l_cs = dynamic_cast<const CoordinateSystem *>(cs->obj.get());
    if (!l_cs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a CoordinateSystem");
        return -1;
    }
    return static_cast<int>(l_cs->axisList().size());
}

// ---------------------------------------------------------------------------

/** \brief Returns information on an axis
 *
 * @param ctx PROJ context, or NULL for default context
 * @param cs Objet of type CoordinateSystem (must not be NULL)
 * @param index Index of the coordinate system (between 0 and
 * proj_obj_cs_get_axis_count() - 1)
 * @param out_name Pointer to a string value to store the axis name. or NULL
 * @param out_abbrev Pointer to a string value to store the axis abbreviation.
 * or NULL
 * @param out_direction Pointer to a string value to store the axis direction.
 * or NULL
 * @param out_unit_conv_factor Pointer to a double value to store the axis
 * unit conversion factor. or NULL
 * @param out_unit_name Pointer to a string value to store the axis
 * unit name. or NULL
 * @param out_unit_auth_name Pointer to a string value to store the axis
 * unit authority name. or NULL
 * @param out_unit_code Pointer to a string value to store the axis
 * unit code. or NULL
 * @return TRUE in case of success
 */
int proj_obj_cs_get_axis_info(PJ_CONTEXT *ctx, const PJ_OBJ *cs, int index,
                              const char **out_name, const char **out_abbrev,
                              const char **out_direction,
                              double *out_unit_conv_factor,
                              const char **out_unit_name,
                              const char **out_unit_auth_name,
                              const char **out_unit_code) {
    SANITIZE_CTX(ctx);
    assert(cs);
    auto l_cs = dynamic_cast<const CoordinateSystem *>(cs->obj.get());
    if (!l_cs) {
        proj_log_error(ctx, __FUNCTION__, "Object is not a CoordinateSystem");
        return false;
    }
    const auto &axisList = l_cs->axisList();
    if (index < 0 || static_cast<size_t>(index) >= axisList.size()) {
        proj_log_error(ctx, __FUNCTION__, "Invalid index");
        return false;
    }
    const auto &axis = axisList[index];
    if (out_name) {
        *out_name = axis->nameStr().c_str();
    }
    if (out_abbrev) {
        *out_abbrev = axis->abbreviation().c_str();
    }
    if (out_direction) {
        *out_direction = axis->direction().toString().c_str();
    }
    if (out_unit_conv_factor) {
        *out_unit_conv_factor = axis->unit().conversionToSI();
    }
    if (out_unit_name) {
        *out_unit_name = axis->unit().name().c_str();
    }
    if (out_unit_auth_name) {
        *out_unit_auth_name = axis->unit().codeSpace().c_str();
    }
    if (out_unit_code) {
        *out_unit_code = axis->unit().code().c_str();
    }
    return true;
}
