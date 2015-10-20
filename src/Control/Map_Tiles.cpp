﻿#include "stdafx.h"
#include "map.h"
#include "GeoPoint.h"
#include "TileHelper.h"
#include "ProjectionHelper.h"
#include "Tiles.h"
#include "WmsHelper.h"

// ************************************************************
//		get_TileManager
// ************************************************************
TileManager* CMapView::get_TileManager()
{
	return ((CTiles*)_tiles)->get_Manager();
}

// ************************************************************
//		GetTilesForMap
// ************************************************************
// Returns zoom level and indices to be loaded for the specific provider, given current map extents.
bool CMapView::get_TilesForMap(void* p, double scalingRatio, CRect& indices, int& zoom)
{
	BaseProvider* provider = reinterpret_cast<BaseProvider*>(p);
	if (!provider) {
		return false;
	}

	// no need to go any further there is no projection
	if (_transformationMode == tmNotDefined) {
		return false;
	}

	Extent clipExtents = _extents;
	bool clipForTiles = get_TileProviderBounds(provider, clipExtents);
	
	Extent bounds;
	GetGeographicExtentsInternal(clipForTiles, &clipExtents, bounds);

	zoom = ChooseZoom(provider, bounds, scalingRatio, true);

	provider->get_Projection()->getTileRectXY(bounds, zoom, indices);

	// some projections may return unpredictable results;
	// so let's put some safeguard against extreme situation (like downloading a million of tiles)
	if (indices.Width() > 50 || indices.Height() > 50) 
	{
		CString s = "Too many tiles are requested for map display. "
					"It may be caused by irregularities of coordinate transformation "
					"(we don't have a reliable method to determine coordinate range "
					"where certain projection use is acceptable).";

		CallbackHelper::ErrorMsg(s);

		return false;
	}

	return true;
}

// ************************************************************
//		ChooseZoom()
// ************************************************************
int CMapView::ChooseZoom(BaseProvider* provider, Extent ext, double scalingRatio, bool limitByProvider)
{
	if (!provider) return 1;

	Point2D center = ext.GetCenter();
	PointLatLng location(center.y, center.x);

	bool precise = _tileProjectionState == ProjectionMatch;
	double ratio = precise ? 0.99 : 0.90;		// 0.99 = set some error margin for rounding issues

	int bestZoom = provider->get_MinZoom();

	for (int i = provider->get_MinZoom(); i <= (limitByProvider ? provider->get_MaxZoom() : 20); i++)
	{
		VARIANT_BOOL isSame = precise ? VARIANT_TRUE : VARIANT_FALSE;

		double tileSize = TileHelper::GetTileSizeProj(isSame, provider, GetWgs84ToMapTransform(), location, i);
		if (tileSize == -1) {
			continue;
		}

		tileSize *= PixelsPerMapUnit();

		int minSize = (int)(256 * scalingRatio  * ratio);
		if (tileSize < minSize) {
			break;
		}

		bestZoom = i;
	}

	CSize s1, s2;
	provider->get_Projection()->GetTileMatrixMinXY(bestZoom, s1);
	provider->get_Projection()->GetTileMatrixMaxXY(bestZoom, s2);

	return bestZoom;
}

// ************************************************************
//		ChooseZoom()
// ************************************************************
int CMapView::ChooseZoom(void* provider, double scalingRatio, bool limitByProvider)
{
	if (!provider) return -1;

	Extent bounds;
	if (GetGeographicExtentsInternal(false, NULL, bounds))
	{
		return ChooseZoom((BaseProvider*)provider, bounds, scalingRatio, limitByProvider);
	}

	return -1;
}

// ************************************************************
//		ProjectionBounds
// ************************************************************
// Returns bounds of the tile provider under current map projection
bool CMapView::get_TileProviderBounds(BaseProvider* provider, Extent& retVal)
{
	if (!provider)	return false;

	BaseProjection* proj = provider->get_Projection();

	if (_projectionChangeCount == proj->MapProjectionCount)
	{
		// map projection hasn't changed from the last calculation,
		// bounds can be reused
		retVal = proj->MapBounds;
		return true;
	}

	double left = proj->get_MinLong();
	double right = proj->get_MaxLong();
	double top = proj->get_MaxLat();
	double bottom = proj->get_MinLat();

	if (_transformationMode == tmDoTransformation)	// i.e. map cs isn't in decimal degrees
	{
		// There is a problem if map projection isn't world wide (like Amersfoort for example).
		// Then values outside its bounds may not to be transformed correctly.
		// There is hardly any workaround here. Ideally we should know the bounds for map
		// projection and clip both by them and by bounds of server projection. Since bounds
		// of map projection aren't available partial solutions can be used:
		// - don't use clipping if map projection isn't world wide while server projection is
		// (which will obviously lead to server bounds outside transformation range).
		// Alternatives:
		// - doing some checks after transformation to make sure that calculations make sense;
		// - add a method to API to set bounds of map projection/tiles;
		// - store and update built-in database of bounds for different coordinate systems
		// and identify projection on setting it to map;
		bool supportsWorldWideTransform = ProjectionHelper::SupportsWorldWideTransform(_projection, _wgsProjection);

		if (proj->IsWorldWide() && !supportsWorldWideTransform) // server projection is world wide, map projection - not
		{
			// so far just skip it;
			// optionally possible to transform to check if the results make sense
			return false;
		}
		else
		{
			VARIANT_BOOL vb;

			_wgsProjection->Transform(&left, &top, &vb);
			if (!vb) {
				Debug::WriteLine("Failed to project: x = %f; y = %f", left, top);
				return false;
			}

			_wgsProjection->Transform(&right, &bottom, &vb);
			if (!vb) {
				Debug::WriteLine("Failed to project: x = %f; y = %f", bottom, right);
				return false;
			}
		}

		//Debug::WriteLine("Projected world bounds: left = %f; right = %f; bottom = %f; top = %f", left, right, bottom, top);
	}

	retVal.left = left;
	retVal.right = right;
	retVal.top = top;
	retVal.bottom = bottom;

	// cache bounds for the further reuse
	proj->MapProjectionCount = _projectionChangeCount;
	proj->MapBounds = retVal;

	return true;
}

// *****************************************************
//		UpdateTileProjection()
// *****************************************************
void CMapView::UpdateTileProjection()
{
	// TODO: use WMS projection instances for zoom selection instead

	VARIANT_BOOL vb;
	_tileProjection->Clear(&vb);
	_tileReverseProjection->Clear(&vb);

	IGeoProjection* gp = NULL;
	_tiles->get_ServerProjection(&gp);

	if (gp) {
		_tileProjection->CopyFrom(gp, &vb);
		gp->Release();
	}
	else {
		CallbackHelper::ErrorMsg("Failed to retrieve server projection for tiles.");
		return;
	}

	_tileReverseProjection->CopyFrom(_projection, &vb);

	if (ProjectionHelper::IsEmpty(_projection))
	{
		_tileProjectionState = ProjectionDoTransform;
		return;
	}

	_tileProjection->get_IsSame(_projection, &vb);
	_tileProjectionState = vb ? ProjectionMatch : ProjectionDoTransform;

	VARIANT_BOOL sphericalMercator;
	_tiles->get_ProjectionIsSphericalMercator(&sphericalMercator);
	
	if (_transformationMode == tmWgs84Complied && sphericalMercator)
	{
		// transformation is needed, but it leads only to some vertical scaling which is quite acceptable
		_tileProjectionState = ProjectionCompatible;
	}

	if (_tileProjectionState == ProjectionDoTransform || _tileProjectionState == ProjectionCompatible)
	{
		CComBSTR bstr;
		_tileProjection->ExportToProj4(&bstr);

		_tileProjection->StartTransform(_projection, &vb);
		if (!vb) {
			ErrorMessage(tkTILES_MAP_TRANSFORM_FAILED);
		}

		_tileReverseProjection->StartTransform(_tileProjection, &vb);
		if (!vb) {
			ErrorMessage(tkMAP_TILES_TRANSFORM_FAILED);
		}
	}
}

// ****************************************************************
//		TileProvider()
// ****************************************************************
void CMapView::SetTileProvider(tkTileProvider provider)
{
	tkTileProvider oldProvider = GetTileProvider();
	if (provider != oldProvider)
	{
		if (provider == tkTileProvider::ProviderNone) {
			_tiles->put_Visible(VARIANT_FALSE);
		}
		else {
			_tiles->put_Provider(provider);
			_tiles->put_Visible(VARIANT_TRUE);
		}

		RedrawCore(RedrawMinimal, false, true);
	}
}

// ****************************************************************
//		GetTileProvider()
// ****************************************************************
tkTileProvider CMapView::GetTileProvider()
{
	VARIANT_BOOL vb;
	_tiles->get_Visible(&vb);
	if (!vb) {
		return tkTileProvider::ProviderNone;
	}
	else
	{
		tkTileProvider provider;
		_tiles->get_Provider(&provider);
		return provider;
	}
}

// ***************************************************************
//		ReloadTiles
// ***************************************************************
void CMapView::ReloadTiles(bool force /*= true*/, bool snapshot /*= false*/, CString key /*= ""*/)
{
	if (force)
	{
		((CTiles*)_tiles)->LoadTiles(snapshot, key);

		ReloadWmsLayers(snapshot, key);
	}
	else 
	{
		// for example provider has changed
		CTiles* tiles = (CTiles*)_tiles;
		if (tiles->get_ReloadNeeded())
		{
			tiles->LoadTiles(snapshot, key);
		}
	}
}

// ***************************************************************
//		ResizeWmsLayerBuffers
// ***************************************************************
void CMapView::ResizeWmsLayerBuffers(int cx, int cy)
{
	// resizing buffers for WMS layers
	for (long i = 0; i < GetNumLayers(); i++)
	{
		Layer* layer = get_LayerByPosition(i);

		if (!layer || layer->IsEmpty() || !layer->IsWmsLayer()) {
			continue;
		}

		CComPtr<IWmsLayer> wms = NULL;
		layer->QueryWmsLayer(&wms);

		WmsHelper::Cast(wms)->ResizeBuffer(cx, cy);
	}
}

// ***************************************************************
//		ReloadWmsLayers
// ***************************************************************
void CMapView::ReloadWmsLayers(bool snapshot, CString key)
{
	double scale = GetCurrentScale();

	// WMS layers
	for (long i = 0; i < GetNumLayers(); i++)
	{
		Layer* layer = get_LayerByPosition(i);

		if (!layer || layer->IsEmpty() || !layer->IsWmsLayer()) {
			continue;
		}

		if (!layer->IsVisible(scale, _currentZoom)) {
			continue;
		}

		CComPtr<IWmsLayer> wms = NULL;
		layer->QueryWmsLayer(&wms);

		WmsHelper::Cast(wms)->Load(this, snapshot, key);
	}

	// TODO: fire TilesLoaded event only once, when the last layer is loaded
	// fire events for individual layers internally
}

// ***************************************************************
//		TilesAreInCache
// ***************************************************************
bool CMapView::TilesAreInCache()
{
	if (!((CTiles*)_tiles)->TilesAreInCache(this))
	{
		return false;
	}

	// TODO: check for WMS layers

	return true;
}
