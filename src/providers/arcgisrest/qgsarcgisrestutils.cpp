/***************************************************************************
    qgsarcgisrestutils.cpp
    ----------------------
    begin                : Nov 25, 2015
    copyright            : (C) 2015 by Sandro Mani
    email                : manisandro@gmail.com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsarcgisrestutils.h"
#include "qgsfield.h"
#include "qgslogger.h"
#include "qgsnetworkaccessmanager.h"
#include "qgsrectangle.h"
#include "geometry/qgsabstractgeometry.h"
#include "geometry/qgscircularstring.h"
#include "geometry/qgscompoundcurve.h"
#include "geometry/qgscurvepolygon.h"
#include "geometry/qgslinestring.h"
#include "geometry/qgsmultipoint.h"
#include "geometry/qgsmulticurve.h"
#include "geometry/qgspolygon.h"
#include "geometry/qgspointv2.h"

#include <QEventLoop>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>


QVariant::Type QgsArcGisRestUtils::mapEsriFieldType( const QString &esriFieldType )
{
  if ( esriFieldType == "esriFieldTypeInteger" )
    return QVariant::LongLong;
  if ( esriFieldType == "esriFieldTypeSmallInteger" )
    return QVariant::Int;
  if ( esriFieldType == "esriFieldTypeDouble" )
    return QVariant::Double;
  if ( esriFieldType == "esriFieldTypeSingle" )
    return QVariant::Double;
  if ( esriFieldType == "esriFieldTypeString" )
    return QVariant::String;
  if ( esriFieldType == "esriFieldTypeDate" )
    return QVariant::Date;
  if ( esriFieldType == "esriFieldTypeGeometry" )
    return QVariant::Invalid; // Geometry column should not appear as field
  if ( esriFieldType == "esriFieldTypeOID" )
    return QVariant::LongLong;
  if ( esriFieldType == "esriFieldTypeBlob" )
    return QVariant::ByteArray;
  if ( esriFieldType == "esriFieldTypeGlobalID" )
    return QVariant::String;
  if ( esriFieldType == "esriFieldTypeRaster" )
    return QVariant::ByteArray;
  if ( esriFieldType == "esriFieldTypeGUID" )
    return QVariant::String;
  if ( esriFieldType == "esriFieldTypeXML" )
    return QVariant::String;
  return QVariant::Invalid;
}

QgsWkbTypes::Type QgsArcGisRestUtils::mapEsriGeometryType( const QString& esriGeometryType )
{
  // http://resources.arcgis.com/en/help/arcobjects-cpp/componenthelp/index.html#//000w0000001p000000
  if ( esriGeometryType == "esriGeometryNull" )
    return QgsWkbTypes::Unknown;
  else if ( esriGeometryType == "esriGeometryPoint" )
    return QgsWkbTypes::Point;
  else if ( esriGeometryType == "esriGeometryMultipoint" )
    return QgsWkbTypes::MultiPoint;
  else if ( esriGeometryType == "esriGeometryPolyline" )
    return QgsWkbTypes::MultiCurve;
  else if ( esriGeometryType == "esriGeometryPolygon" )
    return QgsWkbTypes::Polygon;
  else if ( esriGeometryType == "esriGeometryEnvelope" )
    return QgsWkbTypes::Polygon;
  // Unsupported (either by qgis, or format unspecified by the specification)
  //  esriGeometryCircularArc
  //  esriGeometryEllipticArc
  //  esriGeometryBezier3Curve
  //  esriGeometryPath
  //  esriGeometryRing
  //  esriGeometryLine
  //  esriGeometryAny
  //  esriGeometryMultiPatch
  //  esriGeometryTriangleStrip
  //  esriGeometryTriangleFan
  //  esriGeometryRay
  //  esriGeometrySphere
  //  esriGeometryTriangles
  //  esriGeometryBag
  return QgsWkbTypes::Unknown;
}

static QgsPointV2* parsePoint( const QVariantList& coordList, QgsWkbTypes::Type pointType )
{
  int nCoords = coordList.size();
  if ( nCoords < 2 )
    return nullptr;
  bool xok = false, yok = false;
  double x = coordList[0].toDouble( &xok );
  double y = coordList[1].toDouble( &yok );
  if ( !xok || !yok )
    return nullptr;
  double z = nCoords >= 3 ? coordList[2].toDouble() : 0;
  double m = nCoords >= 4 ? coordList[3].toDouble() : 0;
  return new QgsPointV2( pointType, x, y, z, m );
}

static QgsCircularString* parseCircularString( const QVariantMap& curveData, QgsWkbTypes::Type pointType, const QgsPointV2& startPoint )
{
  QVariantList coordsList = curveData["c"].toList();
  if ( coordsList.isEmpty() )
    return nullptr;
  QList<QgsPointV2> points;
  points.append( startPoint );
  foreach ( const QVariant& coordData, coordsList )
  {
    QgsPointV2* point = parsePoint( coordData.toList(), pointType );
    if ( !point )
    {
      return nullptr;
    }
    points.append( *point );
    delete point;
  }
  QgsCircularString* curve = new QgsCircularString();
  curve->setPoints( points );
  return curve;
}

static QgsCompoundCurve* parseCompoundCurve( const QVariantList& curvesList, QgsWkbTypes::Type pointType )
{
  // [[6,3],[5,3],{"b":[[3,2],[6,1],[2,4]]},[1,2],{"c": [[3,3],[1,4]]}]
  QgsCompoundCurve* compoundCurve = new QgsCompoundCurve();
  QgsLineString* lineString = new QgsLineString();
  compoundCurve->addCurve( lineString );
  foreach ( const QVariant& curveData, curvesList )
  {
    if ( curveData.type() == QVariant::List )
    {
      QgsPointV2* point = parsePoint( curveData.toList(), pointType );
      if ( !point )
      {
        delete compoundCurve;
        return nullptr;
      }
      lineString->addVertex( *point );
      delete point;
    }
    else if ( curveData.type() == QVariant::Map )
    {
      // The last point of the linestring is the start point of this circular string
      QgsCircularString* circularString = parseCircularString( curveData.toMap(), pointType, lineString->endPoint() );
      if ( !circularString )
      {
        delete compoundCurve;
        return nullptr;
      }

      // If the previous curve had less than two points, remove it
      if ( compoundCurve->curveAt( compoundCurve->nCurves() - 1 )->nCoordinates() < 2 )
        compoundCurve->removeCurve( compoundCurve->nCurves() - 1 );

      compoundCurve->addCurve( circularString );

      // Prepare a new line string
      lineString = new QgsLineString;
      compoundCurve->addCurve( lineString );
      lineString->addVertex( circularString->endPoint() );
    }
  }
  return compoundCurve;
}

static QgsAbstractGeometry* parseEsriGeometryPoint( const QVariantMap& geometryData, QgsWkbTypes::Type pointType )
{
  // {"x" : <x>, "y" : <y>, "z" : <z>, "m" : <m>}
  bool xok = false, yok = false;
  double x = geometryData["x"].toDouble( &xok );
  double y = geometryData["y"].toDouble( &yok );
  if ( !xok || !yok )
    return nullptr;
  double z = geometryData["z"].toDouble();
  double m = geometryData["m"].toDouble();
  return new QgsPointV2( pointType, x, y, z, m );
}

static QgsAbstractGeometry* parseEsriGeometryMultiPoint( const QVariantMap& geometryData, QgsWkbTypes::Type pointType )
{
  // {"points" : [[ <x1>, <y1>, <z1>, <m1> ] , [ <x2>, <y2>, <z2>, <m2> ], ... ]}
  QVariantList coordsList = geometryData["points"].toList();
  if ( coordsList.isEmpty() )
    return nullptr;

  QgsMultiPointV2* multiPoint = new QgsMultiPointV2();
  foreach ( QVariant coordData, coordsList )
  {
    QVariantList coordList = coordData.toList();
    QgsPointV2* p = parsePoint( coordList, pointType );
    if ( !p )
    {
      delete multiPoint;
      return nullptr;
    }
    multiPoint->addGeometry( p );
  }
  return multiPoint;
}

static QgsAbstractGeometry* parseEsriGeometryPolyline( const QVariantMap& geometryData, QgsWkbTypes::Type pointType )
{
  // {"curvePaths": [[[0,0], {"c": [[3,3],[1,4]]} ]]}
  QVariantList pathsList;
  if ( geometryData["paths"].isValid() )
    pathsList = geometryData["paths"].toList();
  else if ( geometryData["curvePaths"].isValid() )
    pathsList = geometryData["curvePaths"].toList();
  if ( pathsList.isEmpty() )
    return nullptr;
  QgsMultiCurve* multiCurve = new QgsMultiCurve();
  foreach ( const QVariant& pathData, pathsList )
  {
    QgsCompoundCurve* curve = parseCompoundCurve( pathData.toList(), pointType );
    if ( !curve )
    {
      delete multiCurve;
      return nullptr;
    }
    multiCurve->addGeometry( curve );
  }
  return multiCurve;
}

static QgsAbstractGeometry* parseEsriGeometryPolygon( const QVariantMap& geometryData, QgsWkbTypes::Type pointType )
{
  // {"curveRings": [[[0,0], {"c": [[3,3],[1,4]]} ]]}
  QVariantList ringsList;
  if ( geometryData["rings"].isValid() )
    ringsList = geometryData["rings"].toList();
  else if ( geometryData["ringPaths"].isValid() )
    ringsList = geometryData["ringPaths"].toList();
  if ( ringsList.isEmpty() )
    return nullptr;
  QgsCurvePolygon* polygon = new QgsCurvePolygon();
  QgsCompoundCurve* ext = parseCompoundCurve( ringsList.front().toList(), pointType );
  if ( !ext )
  {
    delete polygon;
    return nullptr;
  }
  polygon->setExteriorRing( ext );
  for ( int i = 1, n = ringsList.size(); i < n; ++i )
  {
    QgsCompoundCurve* curve = parseCompoundCurve( ringsList[i].toList(), pointType );
    if ( !curve )
    {
      delete polygon;
      return nullptr;
    }
    polygon->addInteriorRing( curve );
  }
  return polygon;
}

static QgsAbstractGeometry* parseEsriEnvelope( const QVariantMap& geometryData )
{
  // {"xmin" : -109.55, "ymin" : 25.76, "xmax" : -86.39, "ymax" : 49.94}
  bool xminOk = false, yminOk = false, xmaxOk = false, ymaxOk = false;
  double xmin = geometryData["xmin"].toDouble( &xminOk );
  double ymin = geometryData["ymin"].toDouble( &yminOk );
  double xmax = geometryData["xmax"].toDouble( &xmaxOk );
  double ymax = geometryData["ymax"].toDouble( &ymaxOk );
  if ( !xminOk || !yminOk || !xmaxOk || !ymaxOk )
    return nullptr;
  QgsLineString* ext = new QgsLineString();
  ext->addVertex( QgsPointV2( xmin, ymin ) );
  ext->addVertex( QgsPointV2( xmax, ymin ) );
  ext->addVertex( QgsPointV2( xmax, ymax ) );
  ext->addVertex( QgsPointV2( xmin, ymax ) );
  ext->addVertex( QgsPointV2( xmin, ymin ) );
  QgsPolygonV2* poly = new QgsPolygonV2();
  poly->setExteriorRing( ext );
  return poly;
}

QgsAbstractGeometry* QgsArcGisRestUtils::parseEsriGeoJSON( const QVariantMap& geometryData, const QString& esriGeometryType, bool readM, bool readZ, QgsCoordinateReferenceSystem *crs )
{
  QgsWkbTypes::Type pointType = QgsWkbTypes::zmType( QgsWkbTypes::Point, readZ, readM );
  if ( crs )
  {
    *crs = parseSpatialReference( geometryData["spatialReference"].toMap() );
  }

  // http://resources.arcgis.com/en/help/arcgis-rest-api/index.html#/Geometry_Objects/02r3000000n1000000/
  if ( esriGeometryType == "esriGeometryNull" )
    return nullptr;
  else if ( esriGeometryType == "esriGeometryPoint" )
    return parseEsriGeometryPoint( geometryData, pointType );
  else if ( esriGeometryType == "esriGeometryMultipoint" )
    return parseEsriGeometryMultiPoint( geometryData, pointType );
  else if ( esriGeometryType == "esriGeometryPolyline" )
    return parseEsriGeometryPolyline( geometryData, pointType );
  else if ( esriGeometryType == "esriGeometryPolygon" )
    return parseEsriGeometryPolygon( geometryData, pointType );
  else if ( esriGeometryType == "esriGeometryEnvelope" )
    return parseEsriEnvelope( geometryData );
  // Unsupported (either by qgis, or format unspecified by the specification)
  //  esriGeometryCircularArc
  //  esriGeometryEllipticArc
  //  esriGeometryBezier3Curve
  //  esriGeometryPath
  //  esriGeometryRing
  //  esriGeometryLine
  //  esriGeometryAny
  //  esriGeometryMultiPatch
  //  esriGeometryTriangleStrip
  //  esriGeometryTriangleFan
  //  esriGeometryRay
  //  esriGeometrySphere
  //  esriGeometryTriangles
  //  esriGeometryBag
  return nullptr;
}

QgsCoordinateReferenceSystem QgsArcGisRestUtils::parseSpatialReference( const QVariantMap &spatialReferenceMap )
{
  QString spatialReference = spatialReferenceMap["latestWkid"].toString();
  if ( spatialReference.isEmpty() )
    spatialReference = spatialReferenceMap["wkid"].toString();
  if ( spatialReference.isEmpty() )
    spatialReference = spatialReferenceMap["wkt"].toString();
  else
    spatialReference = QString( "EPSG:%1" ).arg( spatialReference );
  QgsCoordinateReferenceSystem crs;
  crs.createFromString( spatialReference );
  if ( crs.authid().startsWith( "USER:" ) )
    crs.createFromString( "EPSG:4326" ); // If we can't recognize the SRS, fall back to WGS84
  return crs;
}


QVariantMap QgsArcGisRestUtils::getServiceInfo( const QString& baseurl, QString& errorTitle, QString& errorText )
{
  // http://sampleserver5.arcgisonline.com/arcgis/rest/services/Energy/Geology/FeatureServer?f=json
  QUrl queryUrl( baseurl );
  queryUrl.addQueryItem( "f", "json" );
  return queryServiceJSON( queryUrl, errorTitle, errorText );
}

QVariantMap QgsArcGisRestUtils::getLayerInfo( const QString& layerurl, QString& errorTitle, QString& errorText )
{
  // http://sampleserver5.arcgisonline.com/arcgis/rest/services/Energy/Geology/FeatureServer/1?f=json
  QUrl queryUrl( layerurl );
  queryUrl.addQueryItem( "f", "json" );
  return queryServiceJSON( queryUrl, errorTitle, errorText );
}

QVariantMap QgsArcGisRestUtils::getObjectIds( const QString& layerurl, QString& errorTitle, QString& errorText )
{
  // http://sampleserver5.arcgisonline.com/arcgis/rest/services/Energy/Geology/FeatureServer/1/query?where=objectid%3Dobjectid&returnIdsOnly=true&f=json
  QUrl queryUrl( layerurl + "/query" );
  queryUrl.addQueryItem( "f", "json" );
  queryUrl.addQueryItem( "where", "objectid=objectid" );
  queryUrl.addQueryItem( "returnIdsOnly", "true" );
  return queryServiceJSON( queryUrl, errorTitle, errorText );
}

QVariantMap QgsArcGisRestUtils::getObjects( const QString& layerurl, const QList<quint32>& objectIds, const QString &crs,
    bool fetchGeometry, const QStringList& fetchAttributes,
    bool fetchM, bool fetchZ,
    const QgsRectangle& filterRect,
    QString& errorTitle, QString& errorText )
{
  QStringList ids;
  foreach ( int id, objectIds )
  {
    ids.append( QString::number( id ) );
  }
  QUrl queryUrl( layerurl + "/query" );
  queryUrl.addQueryItem( "f", "json" );
  queryUrl.addQueryItem( "objectIds", ids.join( "," ) );
  QString wkid = crs.indexOf( ":" ) >= 0 ? crs.split( ":" )[1] : "";
  queryUrl.addQueryItem( "inSR", wkid );
  queryUrl.addQueryItem( "outSR", wkid );
  QString outFields = fetchAttributes.join( "," );
  if ( fetchGeometry )
  {
    queryUrl.addQueryItem( "returnGeometry", "true" );
    queryUrl.addQueryItem( "outFields", outFields );
  }
  else
  {
    queryUrl.addQueryItem( "returnGeometry", "false" );
    queryUrl.addQueryItem( "outFields", outFields );
  }
  queryUrl.addQueryItem( "returnM", fetchM ? "true" : "false" );
  queryUrl.addQueryItem( "returnZ", fetchZ ? "true" : "false" );
  if ( !filterRect.isEmpty() )
  {
    queryUrl.addQueryItem( "geometry", QString( "%1,%2,%3,%4" )
                           .arg( filterRect.xMinimum(), 0, 'f', -1 ).arg( filterRect.yMinimum(), 0, 'f', -1 )
                           .arg( filterRect.xMaximum(), 0, 'f', -1 ).arg( filterRect.yMaximum(), 0, 'f', -1 ) );
    queryUrl.addQueryItem( "geometryType", "esriGeometryEnvelope" );
    queryUrl.addQueryItem( "spatialRel", "esriSpatialRelEnvelopeIntersects" );
  }
  return queryServiceJSON( queryUrl, errorTitle, errorText );
}

QByteArray QgsArcGisRestUtils::queryService( const QUrl& url, QString& errorTitle, QString& errorText )
{
  QEventLoop loop;

  QNetworkRequest request( url );
  QNetworkReply* reply = 0;
  QgsNetworkAccessManager* nam = QgsNetworkAccessManager::instance();

  // Request data, handling redirects
  while ( true )
  {
    reply = nam->get( request );
    QObject::connect( reply, SIGNAL( finished() ), &loop, SLOT( quit() ) );

    loop.exec( QEventLoop::ExcludeUserInputEvents );

    reply->deleteLater();

    // Handle network errors
    if ( reply->error() != QNetworkReply::NoError )
    {
      QgsDebugMsg( QString( "Network error: %1" ).arg( reply->errorString() ) );
      errorTitle = "Network error";
      errorText = reply->errorString();
      return QByteArray();
    }

    // Handle HTTP redirects
    QVariant redirect = reply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    if ( redirect.isNull() )
    {
      break;
    }

    QgsDebugMsg( "redirecting to " + redirect.toUrl().toString() );
    request.setUrl( redirect.toUrl() );
  }
  QByteArray result = reply->readAll();
  return result;
}

QVariantMap QgsArcGisRestUtils::queryServiceJSON( const QUrl &url, QString &errorTitle, QString &errorText )
{
  QByteArray reply = queryService( url, errorTitle, errorText );
  if ( !errorTitle.isEmpty() )
  {
    return QVariantMap();
  }

  // Parse data
  QJsonParseError err;
  QJsonDocument doc = QJsonDocument::fromJson( reply, &err );
  if ( doc.isNull() )
  {
    errorTitle = "Parsing error";
    errorText = err.errorString();
    QgsDebugMsg( QString( "Parsing error: %1" ).arg( err.errorString() ) );
    return QVariantMap();
  }
  return doc.object().toVariantMap();
}

///////////////////////////////////////////////////////////////////////////////

QgsArcGisAsyncQuery::QgsArcGisAsyncQuery( QObject* parent )
    : QObject( parent )
    , mReply( nullptr )
    , mResult( nullptr )
{
}

QgsArcGisAsyncQuery::~QgsArcGisAsyncQuery()
{
  if ( mReply )
    mReply->deleteLater();
}

void QgsArcGisAsyncQuery::start( const QUrl &url, QByteArray *result, bool allowCache )
{
  mResult = result;
  QNetworkRequest request( url );
  if ( allowCache )
  {
    request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache );
    request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
  }
  mReply = QgsNetworkAccessManager::instance()->get( request );
  connect( mReply, SIGNAL( finished() ), this, SLOT( handleReply() ) );
}

void QgsArcGisAsyncQuery::handleReply()
{
  mReply->deleteLater();
  // Handle network errors
  if ( mReply->error() != QNetworkReply::NoError )
  {
    QgsDebugMsg( QString( "Network error: %1" ).arg( mReply->errorString() ) );
    emit failed( "Network error", mReply->errorString() );
    return;
  }

  // Handle HTTP redirects
  QVariant redirect = mReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
  if ( !redirect.isNull() )
  {
    QNetworkRequest request = mReply->request();
    QgsDebugMsg( "redirecting to " + redirect.toUrl().toString() );
    request.setUrl( redirect.toUrl() );
    mReply = QgsNetworkAccessManager::instance()->get( request );
    connect( mReply, SIGNAL( finished() ), this, SLOT( handleReply() ) );
    return;
  }

  *mResult = mReply->readAll();
  mResult = nullptr;
  emit finished();
}

///////////////////////////////////////////////////////////////////////////////

QgsArcGisAsyncParallelQuery::QgsArcGisAsyncParallelQuery( QObject* parent )
    : QObject( parent )
    , mResults( nullptr )
    , mPendingRequests( 0 )
{
}

void QgsArcGisAsyncParallelQuery::start( const QVector<QUrl> &urls, QVector<QByteArray> *results, bool allowCache )
{
  Q_ASSERT( results->size() == urls.size() );
  mResults = results;
  mPendingRequests = mResults->size();
  for ( int i = 0, n = urls.size(); i < n; ++i )
  {
    QNetworkRequest request( urls[i] );
    request.setAttribute( QNetworkRequest::HttpPipeliningAllowedAttribute, true );
    if ( allowCache )
    {
      request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache );
      request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
      request.setRawHeader( "Connection", "keep-alive" );
    }
    QNetworkReply* reply = QgsNetworkAccessManager::instance()->get( request );
    reply->setProperty( "idx", i );
    connect( reply, SIGNAL( finished() ), this, SLOT( handleReply() ) );
  }
}

void QgsArcGisAsyncParallelQuery::handleReply()
{
  QNetworkReply* reply = qobject_cast<QNetworkReply*>( QObject::sender() );
  QVariant redirect = reply->attribute( QNetworkRequest::RedirectionTargetAttribute );
  int idx = reply->property( "idx" ).toInt();
  reply->deleteLater();
  if ( reply->error() != QNetworkReply::NoError )
  {
    // Handle network errors
    mErrors.append( reply->errorString() );
    --mPendingRequests;
  }
  else if ( !redirect.isNull() )
  {
    // Handle HTTP redirects
    QNetworkRequest request = reply->request();
    QgsDebugMsg( "redirecting to " + redirect.toUrl().toString() );
    request.setUrl( redirect.toUrl() );
    reply = QgsNetworkAccessManager::instance()->get( request );
    reply->setProperty( "idx", idx );
    connect( reply, SIGNAL( finished() ), this, SLOT( handleReply() ) );
  }
  else
  {
    // All ok
    ( *mResults )[idx] = reply->readAll();
    --mPendingRequests;
  }
  if ( mPendingRequests == 0 )
  {
    emit finished( mErrors );
    mResults = nullptr;
    mErrors.clear();
  }
}
