#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdarg.h>

#include "MQBasePlugin.h"
#include "MQ3DLib.h"
#include "MQSetting.h"

#include <vector>
#include <map>
#include <set>

void debuglog(MQDocument doc, const char* fmt, ...);

inline bool operator<(const MQSelectVertex& v1, const MQSelectVertex& v2) 
{
	if(v1.object != v2.object) return v1.object < v2.object;
	return v1.vertex < v2.vertex;
}

inline MQCoordinate& operator+=(MQCoordinate& c1, const MQCoordinate& c2)
{
	c1.u += c2.u;
	c1.v += c2.v;
	return c1;
}
inline MQCoordinate& operator-=(MQCoordinate& c1, const MQCoordinate& c2)
{
	c1.u -= c2.u;
	c1.v -= c2.v;
	return c1;
}

static MQCommandPlugin::EDIT_OPTION s_editoption;

enum ObjectEnumerator_SkipOption {
	OE_SKIPLOCKED = 0x1,
	OE_SKIPHIDDEN = 0x2,
	OE_APPLYEDITOPTION = 0x4,
	OE_APPLYALL = 0xff
};

class ObjectEnumerator
{
public:

	ObjectEnumerator(MQDocument doc, DWORD option = OE_APPLYALL)
	{
		m_option = option;
		m_doc = doc;
		m_objcount = doc->GetObjectCount();
		m_cur = -1;
	}
	
	void Reset() { m_cur = -1; }

	int GetIndex() { return m_cur; }

	MQObject next()
	{
		while(1)
		{
			m_cur++;
			if(m_cur >= m_objcount) return NULL;

			MQObject obj = m_doc->GetObject(m_cur);

			if(obj == NULL) continue;
			if(obj->GetVertexCount() == 0) continue;
			if((m_option & OE_SKIPLOCKED) && obj->GetLocking()) continue;
			if((m_option & OE_SKIPHIDDEN) && !obj->GetVisible()) continue;
			if((m_option & OE_APPLYEDITOPTION) && s_editoption.CurrentObjectOnly && m_cur != m_doc->GetCurrentObjectIndex()) continue;

			return obj;	
		}
	}

private:
	ObjectEnumerator() {}

	int m_cur;
	int m_objcount;

	MQDocument m_doc;
	DWORD m_option;
};

enum MQSelectElement_SelectedItemType {
	SELEL_NONE,
	SELEL_VERTEX,
	SELEL_LINE,
	SELEL_FACE,
};

class MQSelectElement
{
public:
	MQSelectElement() 
	{
		type = SELEL_NONE; 
		index_o = -1;
		index_1 = -1;
		index_2 = -1;
	}

	bool IsEmpty() { return (type == SELEL_NONE); }
	void Reset() { type = SELEL_NONE; index_o = -1; index_1 = -1; index_2 = -1; }
	void SetVertex(int o, int v) { type = SELEL_VERTEX; index_o = o; index_1 = v; index_2 = -1; }
	void SetLine(int o, int f, int l) { type = SELEL_LINE; index_o = o; index_1 = f; index_2 = l; }
	void SetFace(int o, int f) { type = SELEL_FACE; index_o = o; index_1 = f; index_2 = -1; }

	int GetType() { return type; }
	int GetObjectIndex() { return index_o; }
	int GetFaceIndex() { return index_1; }
	int GetLineIndex() { return index_2; }
	int GetVertexIndex() { return index_1; }

	bool operator!=(const MQSelectElement& a) 
	{ 
		return (type != a.type) || (index_o != a.index_o) || (index_1 != a.index_1) || (index_2 != a.index_2);
	}
	bool operator==(const MQSelectElement& a) { return !(*this != a ); }

	MQPoint GetPoint(MQDocument doc)
	{
		MQPoint p(0,0,0);
		if(IsEmpty()) return p;
		MQObject obj = doc->GetObject(index_o);
		int indices[5];
		int pcount;
		if(obj == NULL) return p;
		switch(type)
		{
		case SELEL_FACE:
			obj->GetFacePointArray(index_1,indices);
			pcount = obj->GetFacePointCount(index_1);
			for(int i=0;i<pcount;i++) p += obj->GetVertex(indices[i]);
			p /= (float)pcount;
			break;
		case SELEL_LINE:
			obj->GetFacePointArray(index_1,indices);
			pcount = obj->GetFacePointCount(index_1);
			p += obj->GetVertex(indices[index_2]);
			p += obj->GetVertex(indices[(index_2+1)%pcount]);
			p /= 2.0f;
			break;
		case SELEL_VERTEX:
			p = obj->GetVertex(index_1);
			break;
		}
		return p;
	}

	void Select(MQDocument doc)
	{
		switch(type)
		{
		case SELEL_FACE:
			doc->AddSelectFace(index_o,index_1);
			break;
		case SELEL_LINE:
			doc->AddSelectLine(index_o,index_1,index_2); 
			break;
		case SELEL_VERTEX:
			doc->AddSelectVertex(index_o,index_1);
			break;
		}
	}

	void Deselect(MQDocument doc)
	{
		switch(type)
		{
		case SELEL_FACE:
			doc->DeleteSelectFace(index_o,index_1);
			break;
		case SELEL_LINE:
			doc->DeleteSelectLine(index_o,index_1,index_2);
			break;
		case SELEL_VERTEX:
			doc->DeleteSelectVertex(index_o,index_1);
			break;
		}
	}

	bool IsSelected(MQDocument doc)
	{
		switch(type)
		{
		case SELEL_FACE:
			return doc->IsSelectFace(index_o,index_1) == TRUE;
		case SELEL_LINE:
			return doc->IsSelectLine(index_o,index_1,index_2) == TRUE;
		case SELEL_VERTEX:
			return doc->IsSelectVertex(index_o,index_1) == TRUE;
		default:
			return false;
		}
	}

private:
	int type;
	int index_o;
	int index_1;
	int index_2;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

class XUVPlugin : public MQCommandPlugin
{
public:
	XUVPlugin() 
	{
		m_active = false;

		m_panel_scale = 800.0f;
		m_panel_offset.u = 0.0f;
		m_panel_offset.v = 0.0f;

		m_shiftstate = false;
	}
	~XUVPlugin()
	{
	}

	void GetPlugInID(DWORD *Product, DWORD *ID)
	{
		*Product = 0xabcdef0e;
		*ID      = 0x00101013;
	}

	const char *GetPlugInName(void) { return "UV RollingExpand ver.1.0   Copyright(C) 2007 kth "; }
	const char *EnumString(void) { return "UV RollingExpand"; }

	BOOL Initialize() { return TRUE; }
	void Exit() {}

	BOOL Activate(MQDocument doc, BOOL flag);

	BOOL OnMouseMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	void OnDraw(MQDocument doc, MQScene scene, int width, int height);
	BOOL OnLeftButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnLeftButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnLeftButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);

	BOOL OnMiddleButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnMiddleButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnMiddleButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);

	BOOL OnRightButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnRightButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);
	BOOL OnRightButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);


	BOOL OnMouseWheel(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state);

	BOOL OnKeyDown(MQDocument doc, MQScene scene, int key, MOUSE_BUTTON_STATE& state);
	BOOL OnKeyUp(MQDocument doc, MQScene scene, int key, MOUSE_BUTTON_STATE& state);


private:
	void draw_uv_panel(MQDocument doc, MQScene scene);
	void draw_uv_network(MQDocument doc, MQScene scene);
	void zoom_panel(float delta);

	bool m_active;
	bool m_moved;

	bool m_shiftstate;

	POINT m_last_drag_sc;

	MQCoordinate m_dragstart;
	float m_lastdistance;

	float m_panel_scale;
	MQCoordinate m_panel_offset;
	MQColor m_panel_color;

};

static  XUVPlugin s_plugin;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////



static void find_faces_contains_vertex(MQObject obj, int vindex, std::vector<int>& out)
{
	out.reserve(8);
	int fcount = obj->GetFaceCount();
	for(int f = 0; f < fcount; f++)
	{
		int indices[5] = {-1,-1,-1,-1,-1};
		obj->GetFacePointArray(f,indices);
		
		for(int i = 0; indices[i] != -1; i++)
		{
			if(vindex == indices[i]) { out.push_back(f); break; }
		}
	}
}


static void get_selection(MQDocument doc,MQScene scene,std::vector<MQSelectVertex>& out)
{
	int indices[4];

	int obj_count = doc->GetObjectCount();

	std::set<MQSelectVertex> tmp;

	// for each objects
	ObjectEnumerator objenum(doc);
	for(MQObject obj = NULL; (obj = objenum.next()) != NULL;)
	{
		int o = objenum.GetIndex();
		int face_count = obj->GetFaceCount();

		for(int f = 0; f < face_count; f++)
		{
			// pass through if the face is invalid (count of vertex refer is 0)
			int ptcount = obj->GetFacePointCount(f);
			if(ptcount == 0) continue;

			obj->GetFacePointArray(f,indices);

			// selected is face
			if(doc->IsSelectFace(o,f))
			{
				for(int p = 0; p < ptcount; p++)
					tmp.insert(MQSelectVertex(o,indices[p]));
				continue;
			}

			for(int p = 0; p < ptcount; p++)
			{
				if(doc->IsSelectLine(o,f,p))
				{
					tmp.insert(MQSelectVertex(o,indices[p]));
					tmp.insert(MQSelectVertex(o,indices[(p+1)%ptcount]));
				}
				if(doc->IsSelectVertex(o,indices[p]))
				{
					tmp.insert(MQSelectVertex(o,indices[p]));
				}
			}
		}
	}

	for(std::set<MQSelectVertex>::iterator it = tmp.begin(); it != tmp.end(); ++it)
	{
		out.push_back(*it);
	}
}	



///////////////////////////////////////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
BOOL XUVPlugin::Activate(MQDocument doc, BOOL flag)
{
	m_active = (flag == TRUE);

	m_dragstart.u = -1.0f;
	m_moved = false;
	m_last_drag_sc.x = -1;

	m_shiftstate = false;

	if(flag == TRUE)
	{
		this->GetEditOption(s_editoption);

		char path[MAX_PATH];
		if(MQ_GetSystemPath(path, MQFOLDER_METASEQ_INI))
		{
			MQSetting set(path, "Screen");
			unsigned int col;
			set.Load("ColorUV",col,(unsigned int)14270285);
			m_panel_color.b = (float)((col >> 16) & 0xff) / 255.0f;
			m_panel_color.g = (float)((col >> 8) & 0xff) / 255.0f;
			m_panel_color.r = (float)(col & 0xff) / 255.0f;
		}
	}
	else
	{
	}

	RedrawAllScene();
	
	// return flag untouched
	return flag;
}



//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
BOOL XUVPlugin::OnMouseMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	return FALSE;
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
void XUVPlugin::draw_uv_panel(MQDocument doc, MQScene scene)
{
	int matid = doc->GetCurrentMaterialIndex();
	if(matid == -1) return;

	MQPoint dvertices[4];
	MQCoordinate uvarray[4] = 
	{
		MQCoordinate(0,0),
		MQCoordinate(1.0f,0),
		MQCoordinate(1.0f,1.0f),
		MQCoordinate(0,1.0f),
	};

	int dindices[4];
	MQObject dobj = CreateDrawingObject(doc,DRAW_OBJECT_FACE);

	for(int i = 0; i < 4; i++)
	{
		dvertices[i] = MQPoint(uvarray[i].u, uvarray[i].v, 0);
		dvertices[i] *= m_panel_scale;
		dvertices[i].x += m_panel_offset.u;
		dvertices[i].y += m_panel_offset.v;
		dvertices[i].z = 0.99999f;
		dvertices[i] = scene->ConvertScreenTo3D(dvertices[i]);
		dindices[i] = dobj->AddVertex(dvertices[i]);
	}
	
	dobj->AddFace(4,dindices);
	dobj->SetFaceCoordinateArray(0,uvarray);
	dobj->SetFaceMaterial(0,matid);
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
void XUVPlugin::draw_uv_network(MQDocument doc, MQScene scene)
{
	MQObject obj = doc->GetObject(doc->GetCurrentObjectIndex());
	if(obj == NULL) return;

	const float drawz = 0.99998f;

	MQObject dobj_lines = CreateDrawingObject(doc,DRAW_OBJECT_LINE);
	dobj_lines->SetColor(m_panel_color);
	dobj_lines->SetColorValid(TRUE);
	
	int fcount = obj->GetFaceCount();
	for(int f = 0; f < fcount; f++)
	{
		int pcount = obj->GetFacePointCount(f);

		MQCoordinate uvarray[4];
		obj->GetFaceCoordinateArray(f,uvarray);

		MQPoint lastp;
		for(int i = 0; i < pcount; i++)
		{
			MQPoint p(
				uvarray[i].u * m_panel_scale + m_panel_offset.u, 
				uvarray[i].v * m_panel_scale + m_panel_offset.v, 
				drawz);
			p = scene->ConvertScreenTo3D(p);

			if(i != 0)
			{
				int indices[2];
				indices[0] = dobj_lines->AddVertex(lastp);
				indices[1] = dobj_lines->AddVertex(p);
				dobj_lines->AddFace(2,indices);
			}

			lastp = p;
		}
	}
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
void XUVPlugin::OnDraw(MQDocument doc, MQScene scene, int width, int height)
{
	if(!m_active) return;
	//if(scene != doc->GetScene(0)) return;

	// draw uv panel
	draw_uv_panel(doc,scene);

	// draw uv network
	draw_uv_network(doc,scene);


	if(m_shiftstate)
	{
		MQObject dotobj = CreateDrawingObject(doc,DRAW_OBJECT_POINT);
		dotobj->SetColor(MQColor(1,0,1));
		dotobj->SetColorValid(TRUE);
		int i = dotobj->AddVertex(scene->ConvertScreenTo3D(MQPoint(0,0,0.0001f)));
		dotobj->AddFace(1,&i);
	}

	if(m_dragstart.u != -1)
	{
		MQObject dotobj = CreateDrawingObject(doc,DRAW_OBJECT_POINT);
		dotobj->SetColor(MQColor(1,1,0));
		dotobj->SetColorValid(TRUE);
		int i = dotobj->AddVertex(scene->ConvertScreenTo3D(MQPoint(
			m_dragstart.u * m_panel_scale + m_panel_offset.u,
			m_dragstart.v * m_panel_scale + m_panel_offset.v,
			0.9996f)));
		dotobj->AddFace(1,&i);
	}

}


//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
BOOL XUVPlugin::OnLeftButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(doc->GetCurrentMaterialIndex() == -1) return FALSE;
	if(doc->GetObject(doc->GetCurrentObjectIndex()) == NULL) return FALSE;

	m_dragstart.u = (float)(state.MousePos.x - m_panel_offset.u) / m_panel_scale;
	m_dragstart.v = (float)(state.MousePos.y - m_panel_offset.v) / m_panel_scale;

	m_lastdistance = 0.0f;

	return TRUE;
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
BOOL XUVPlugin::OnLeftButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(m_dragstart.u == -1.0f) return FALSE;

	MQObject obj;
	if((obj = doc->GetObject(doc->GetCurrentObjectIndex())) == NULL) return FALSE;

	m_moved = true;

	MQCoordinate drag;
	drag.u = (float)(state.MousePos.x - m_panel_offset.u) / m_panel_scale;
	drag.v = (float)(state.MousePos.y - m_panel_offset.v) / m_panel_scale;
	drag -= m_dragstart;
	float currentdistance = sqrtf(drag.u * drag.u + drag.v * drag.v);
	float move = currentdistance - m_lastdistance;

	if(currentdistance <= 0.000001f) return TRUE;

	// scale down - think it's a bit so strong  
	move *= 0.3f;

	MQCoordinate pressvector(drag.u / currentdistance, drag.v / currentdistance);

	int fcount = obj->GetFaceCount();
	for(int f = 0; f < fcount; f++)
	{
		MQCoordinate uvarray[4];
		int indices[4];
		int pcount = obj->GetFacePointCount(f);
		obj->GetFaceCoordinateArray(f,uvarray);
		obj->GetFacePointArray(f,indices);

		for(int i = 0; i < pcount; i++)
		{
			MQCoordinate coord = uvarray[i];
			coord -= m_dragstart;
			float dist = sqrtf(coord.u * coord.u + coord.v * coord.v);

			// normalize
			coord.u /= dist;
			coord.v /= dist;

			// scalling with dot
			//float dot = (coord.u * pressvector.u + coord.v * pressvector.v);
			//dot = max(0,dot + 0.5f) / 1.5f;
			float dot = 1.0f;

			// scalling of region of effect
			float threshold = dist * m_panel_scale / 700.0f;

			MQCoordinate delta = coord * move * dot *
				max(0, (1.0f - ((threshold*threshold)/currentdistance)));

			uvarray[i] += delta;
		}

		obj->SetFaceCoordinateArray(f,uvarray);
	}

	m_lastdistance = currentdistance;

	RedrawScene(scene);

	return TRUE;
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
BOOL XUVPlugin::OnLeftButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(m_moved)
	{
		RedrawAllScene();
		UpdateUndo();
	}

	m_dragstart.u = -1.0f;
	m_moved = false;

	return TRUE;
}





BOOL XUVPlugin::OnMiddleButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(!state.Shift) return FALSE;

	m_last_drag_sc = state.MousePos;
	return TRUE;
}

BOOL XUVPlugin::OnMiddleButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(m_last_drag_sc.x == -1) return FALSE;

	m_panel_offset.u += (float)(state.MousePos.x - m_last_drag_sc.x);
	m_panel_offset.v += (float)(state.MousePos.y - m_last_drag_sc.y);

	m_last_drag_sc = state.MousePos;

	RedrawScene(scene);

	return TRUE;
}

BOOL XUVPlugin::OnMiddleButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	m_last_drag_sc.x = -1;
	return FALSE;
}

//---------------------------------------------------------------------------------
//---------------------------------------------------------------------------------

void XUVPlugin::zoom_panel(float delta)
{
	if(m_panel_scale < 100.0f && delta < 0.0f) delta = 0.0f;

	float offset_delta = delta * 0.5f;

	m_panel_offset.u -= offset_delta;
	m_panel_offset.v -= offset_delta;

	m_panel_scale += delta;
}


BOOL XUVPlugin::OnRightButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(!state.Shift) return FALSE;
	m_last_drag_sc = state.MousePos;
	return TRUE;
}

BOOL XUVPlugin::OnRightButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if(m_last_drag_sc.x == -1) return FALSE;

	float scale_delta = (float)(state.MousePos.x - m_last_drag_sc.x);

	zoom_panel(scale_delta);

	m_last_drag_sc = state.MousePos;

	RedrawScene(scene);

	return TRUE;
}
BOOL XUVPlugin::OnRightButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	m_last_drag_sc.x = -1;
	return FALSE;
}

BOOL XUVPlugin::OnMouseWheel(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	debuglog(doc,"wheel %d",m_shiftstate?1:0);
	if(!m_shiftstate) return FALSE;

	zoom_panel((state.Wheel < 0)?-60.0f:60.0f);

	RedrawScene(scene);

	return TRUE;
}

BOOL XUVPlugin::OnKeyDown(MQDocument doc, MQScene scene, int key, MOUSE_BUTTON_STATE& state)
{
	if(key == VK_SHIFT && m_shiftstate == false)
	{
		m_shiftstate = true;
		RedrawAllScene();
		return TRUE;
	}
	return FALSE;
}

BOOL XUVPlugin::OnKeyUp(MQDocument doc, MQScene scene, int key, MOUSE_BUTTON_STATE& state)
{
	if(key == VK_SHIFT && m_shiftstate == true)
	{
		m_shiftstate = false;
		RedrawAllScene();
		return TRUE;
	}
	return FALSE;
}




//---------------------------------------------------------------------------
//
//---------------------------------------------------------------------------
MQBasePlugin *GetPluginClass() 
{
	return &s_plugin; 
}

BOOL APIENTRY DllMain( HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved )				 
{
    return TRUE;
}

static void debuglog(MQDocument doc, const char* fmt, ...)
{
	va_list args;
	va_start(args,fmt);
	char buf[512];
	vsnprintf_s(buf,512,100,fmt,args);
	va_end(args);

	s_plugin.SendUserMessage(doc, 0x56A31D20, 0x9CE001E3, "XUV", buf);
}

