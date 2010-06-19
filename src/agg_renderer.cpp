/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2006 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/
//$Id$

// mapnik
#include <mapnik/agg_renderer.hpp>
#include <mapnik/agg_rasterizer.hpp>
#include <mapnik/image_util.hpp>
#include <mapnik/image_cache.hpp>
#include <mapnik/unicode.hpp>
#include <mapnik/placement_finder.hpp>
#include <mapnik/config_error.hpp>
#include <mapnik/font_set.hpp>
#include <mapnik/parse_path.hpp>
#include <mapnik/text_path.hpp>

// agg
#define AGG_RENDERING_BUFFER row_ptr_cache<int8u>
#include "agg_rendering_buffer.h"
#include "agg_pixfmt_rgba.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_basics.h"
#include "agg_scanline_p.h"
#include "agg_scanline_u.h"
#include "agg_renderer_scanline.h"
#include "agg_path_storage.h"
#include "agg_span_allocator.h"
#include "agg_span_pattern_rgba.h"
#include "agg_image_accessors.h"
#include "agg_conv_stroke.h"
#include "agg_conv_dash.h"
#include "agg_conv_contour.h"
#include "agg_conv_clip_polyline.h"
#include "agg_vcgen_stroke.h"
#include "agg_conv_adaptor_vcgen.h"
#include "agg_conv_smooth_poly1.h"
#include "agg_conv_marker.h"
#include "agg_vcgen_markers_term.h"
#include "agg_renderer_outline_aa.h"
#include "agg_rasterizer_outline_aa.h"
#include "agg_rasterizer_outline.h"
#include "agg_renderer_outline_image.h"
#include "agg_span_allocator.h"
#include "agg_span_pattern_rgba.h"
#include "agg_renderer_scanline.h"
#include "agg_pattern_filters_rgba.h"
#include "agg_renderer_outline_image.h"
#include "agg_vpgen_clip_polyline.h"
#include "agg_arrowhead.h"

// boost
#include <boost/utility.hpp>
#include <boost/tuple/tuple.hpp>

// stl
#ifdef MAPNIK_DEBUG
#include <iostream>
#endif

#include <cmath>

namespace mapnik
{
class pattern_source : private boost::noncopyable
{
public:
    pattern_source(image_data_32 const& pattern)
        : pattern_(pattern) {}

    unsigned int width() const
    {
        return pattern_.width();
    }
    unsigned int height() const
    {
        return pattern_.height();
    }
    agg::rgba8 pixel(int x, int y) const
    {
        unsigned c = pattern_(x,y);
        return agg::rgba8(c & 0xff,
                          (c >> 8) & 0xff,
                          (c >> 16) & 0xff,
                          (c >> 24) & 0xff);
    }
private:
    image_data_32 const& pattern_;
};


template <typename T>
agg_renderer<T>::agg_renderer(Map const& m, T & pixmap, double scale_factor, unsigned offset_x, unsigned offset_y)
    : feature_style_processor<agg_renderer>(m, scale_factor),
      pixmap_(pixmap),
      width_(pixmap_.width()),
      height_(pixmap_.height()),
      scale_factor_(scale_factor),
      t_(m.getWidth(),m.getHeight(),m.getCurrentExtent(),offset_x,offset_y),
      font_engine_(),
      font_manager_(font_engine_),
      detector_(box2d<double>(-m.buffer_size(), -m.buffer_size(), m.getWidth() + m.buffer_size() ,m.getHeight() + m.buffer_size())),
      ras_ptr(new rasterizer)
{
    boost::optional<color> bg = m.background();
    if (bg) pixmap_.set_background(*bg);
#ifdef MAPNIK_DEBUG
    std::clog << "scale=" << m.scale() << "\n";
#endif
}

template <typename T>
agg_renderer<T>::~agg_renderer() {}

template <typename T>
void agg_renderer<T>::start_map_processing(Map const& map)
{
#ifdef MAPNIK_DEBUG
    std::clog << "start map processing bbox="
              << map.getCurrentExtent() << "\n";
#endif
    ras_ptr->clip_box(0,0,width_,height_);
}

template <typename T>
void agg_renderer<T>::end_map_processing(Map const& )
{
#ifdef MAPNIK_DEBUG
    std::clog << "end map processing\n";
#endif
}

template <typename T>
void agg_renderer<T>::start_layer_processing(layer const& lay)
{
#ifdef MAPNIK_DEBUG
    std::clog << "start layer processing : " << lay.name()  << "\n";
    std::clog << "datasource = " << lay.datasource().get() << "\n";
#endif
    if (lay.clear_label_cache())
    {
        detector_.clear();
    }
}

template <typename T>
void agg_renderer<T>::end_layer_processing(layer const&)
{
#ifdef MAPNIK_DEBUG
    std::clog << "end layer processing\n";
#endif
}

typedef boost::tuple<double,double,double,double> segment_t;
bool y_order(segment_t const& first,segment_t const& second)
{
    double miny0 = std::min(first.get<1>(),first.get<3>());
    double miny1 = std::min(second.get<1>(),second.get<3>());
    return  miny0 > miny1;
}

template <typename T>
void agg_renderer<T>::process(building_symbolizer const& sym,
                              Feature const& feature,
                              proj_transform const& prj_trans)
{
    typedef  coord_transform2<CoordTransform,geometry2d> path_type;
    typedef  coord_transform3<CoordTransform,geometry2d> path_type_roof;
    typedef agg::renderer_base<agg::pixfmt_rgba32_plain> ren_base;
    typedef agg::renderer_scanline_aa_solid<ren_base> renderer;

    agg::rendering_buffer buf(pixmap_.raw_data(),width_,height_, width_ * 4);
    agg::pixfmt_rgba32_plain pixf(buf);
    ren_base renb(pixf);

    color const& fill_  = sym.get_fill();
    unsigned r=fill_.red();
    unsigned g=fill_.green();
    unsigned b=fill_.blue();
    unsigned a=fill_.alpha();
    renderer ren(renb);
    agg::scanline_u8 sl;

    ras_ptr->reset();
    ras_ptr->gamma(agg::gamma_linear());
    
    double height = 0.7071 * sym.height(); // height in meters

    for (unsigned i=0;i<feature.num_geometries();++i)
    {
        geometry2d const& geom = feature.get_geometry(i);
        if (geom.num_points() > 2)
        {
            boost::scoped_ptr<geometry2d> frame(new line_string_impl);
            boost::scoped_ptr<geometry2d> roof(new polygon_impl);
            std::deque<segment_t> face_segments;
            double x0(0);
            double y0(0);
            unsigned cm = geom.vertex(&x0,&y0);
            for (unsigned j=1;j<geom.num_points();++j)
            {
                double x(0);
                double y(0);
                cm = geom.vertex(&x,&y);
                if (cm == SEG_MOVETO)
                {
                    frame->move_to(x,y);
                }
                else if (cm == SEG_LINETO)
                {
                    frame->line_to(x,y);
                    face_segments.push_back(segment_t(x0,y0,x,y));
                }
                
                x0 = x;
                y0 = y;
            }
            std::sort(face_segments.begin(),face_segments.end(), y_order);
            std::deque<segment_t>::const_iterator itr=face_segments.begin();
            for (;itr!=face_segments.end();++itr)
            {
                boost::scoped_ptr<geometry2d> faces(new polygon_impl);
                faces->move_to(itr->get<0>(),itr->get<1>());
                faces->line_to(itr->get<2>(),itr->get<3>());
                faces->line_to(itr->get<2>(),itr->get<3>() + height);
                faces->line_to(itr->get<0>(),itr->get<1>() + height);

                path_type faces_path (t_,*faces,prj_trans);
                ras_ptr->add_path(faces_path);
                ren.color(agg::rgba8(int(r*0.8), int(g*0.8), int(b*0.8), int(a * sym.get_opacity())));
                agg::render_scanlines(*ras_ptr, sl, ren);
                ras_ptr->reset();

                frame->move_to(itr->get<0>(),itr->get<1>());
                frame->line_to(itr->get<0>(),itr->get<1>()+height);
            }

            geom.rewind(0);
            for (unsigned j=0;j<geom.num_points();++j)
            {
                double x,y;
                unsigned cm = geom.vertex(&x,&y);
                if (cm == SEG_MOVETO)
                {
                    frame->move_to(x,y+height);
                    roof->move_to(x,y+height);
                }
                else if (cm == SEG_LINETO)
                {
                    frame->line_to(x,y+height);
                    roof->line_to(x,y+height);
                }
            }
            path_type path(t_,*frame,prj_trans);
            agg::conv_stroke<path_type>  stroke(path);
            ras_ptr->add_path(stroke);
            ren.color(agg::rgba8(r * 0.8, g * 0.8 , b * 0.8, int(255 * sym.get_opacity())));
            agg::render_scanlines(*ras_ptr, sl, ren);
            ras_ptr->reset();

            path_type roof_path (t_,*roof,prj_trans);
            ras_ptr->add_path(roof_path);
            ren.color(agg::rgba8(r, g, b, int(a * sym.get_opacity())));
            agg::render_scanlines(*ras_ptr, sl, ren);
        }
    }
}


template <typename T>
void  agg_renderer<T>::process(line_pattern_symbolizer const& sym,
                               Feature const& feature,
                               proj_transform const& prj_trans)
{
    typedef  coord_transform2<CoordTransform,geometry2d> path_type;
    typedef agg::line_image_pattern<agg::pattern_filter_bilinear_rgba8> pattern_type;
    typedef agg::renderer_base<agg::pixfmt_rgba32_plain> renderer_base;
    typedef agg::renderer_outline_image<renderer_base, pattern_type> renderer_type;
    typedef agg::rasterizer_outline_aa<renderer_type> rasterizer_type;

    agg::rendering_buffer buf(pixmap_.raw_data(),width_,height_, width_ * 4);
    agg::pixfmt_rgba32_plain pixf(buf);
    
    std::string filename = path_processor_type::evaluate( *sym.get_filename(), feature);
    boost::optional<image_ptr> pat = image_cache::instance()->find(filename,true);

    if (!pat) return;
      
    renderer_base ren_base(pixf);
    agg::pattern_filter_bilinear_rgba8 filter;
    pattern_source source(*(*pat));
    pattern_type pattern (filter,source);
    renderer_type ren(ren_base, pattern);
    ren.clip_box(0,0,width_,height_);
    rasterizer_type ras(ren);
    
    for (unsigned i=0;i<feature.num_geometries();++i)
    {
        geometry2d const& geom = feature.get_geometry(i);
        if (geom.num_points() > 1)
        {
            path_type path(t_,geom,prj_trans);
            ras.add_path(path);
        }
    }
}

template <typename T>
void agg_renderer<T>::process(polygon_pattern_symbolizer const& sym,
                              Feature const& feature,
                              proj_transform const& prj_trans)
{
    typedef coord_transform2<CoordTransform,geometry2d> path_type;
    typedef agg::renderer_base<agg::pixfmt_rgba32_plain> ren_base;
    typedef agg::wrap_mode_repeat wrap_x_type;
    typedef agg::wrap_mode_repeat wrap_y_type;
    typedef agg::pixfmt_alpha_blend_rgba<agg::blender_rgba32,
        agg::row_accessor<agg::int8u>, agg::pixel32_type> rendering_buffer;
    typedef agg::image_accessor_wrap<rendering_buffer,
        wrap_x_type,
        wrap_y_type> img_source_type;

    typedef agg::span_pattern_rgba<img_source_type> span_gen_type;

    typedef agg::renderer_scanline_aa<ren_base,
        agg::span_allocator<agg::rgba8>,
        span_gen_type> renderer_type;


    agg::rendering_buffer buf(pixmap_.raw_data(),width_,height_, width_ * 4);
    agg::pixfmt_rgba32_plain pixf(buf);
    ren_base renb(pixf);
    
    agg::scanline_u8 sl;
    ras_ptr->reset();
    ras_ptr->gamma(agg::gamma_linear());

    std::string filename = path_processor_type::evaluate( *sym.get_filename(), feature);
    boost::optional<image_ptr> pat = image_cache::instance()->find(filename,true);

    if (!pat) return;
    
    unsigned w=(*pat)->width();
    unsigned h=(*pat)->height();
    agg::row_accessor<agg::int8u> pattern_rbuf((agg::int8u*)(*pat)->getBytes(),w,h,w*4);
    agg::span_allocator<agg::rgba8> sa;
    agg::pixfmt_alpha_blend_rgba<agg::blender_rgba32,
        agg::row_accessor<agg::int8u>, agg::pixel32_type> pixf_pattern(pattern_rbuf);
    img_source_type img_src(pixf_pattern);
    
    double x0=0,y0=0;
    unsigned num_geometries = feature.num_geometries();
    if (num_geometries>0)
    {
        path_type path(t_,feature.get_geometry(0),prj_trans);
        path.vertex(&x0,&y0);
    }
    unsigned offset_x = unsigned(width_-x0);
    unsigned offset_y = unsigned(height_-y0);
    span_gen_type sg(img_src, offset_x, offset_y);
    renderer_type rp(renb,sa, sg);
    for (unsigned i=0;i<num_geometries;++i)
    {
        geometry2d const& geom = feature.get_geometry(i);
        if (geom.num_points() > 2)
        {
            path_type path(t_,geom,prj_trans);
            ras_ptr->add_path(path);
        }
    }
    agg::render_scanlines(*ras_ptr, sl, rp);
}

template <typename T>
void agg_renderer<T>::process(raster_symbolizer const& sym,
                              Feature const& feature,
                              proj_transform const& prj_trans)
{
    raster_ptr const& raster=feature.get_raster();
    if (raster)
    {
        // If there's a colorizer defined, use it to color the raster in-place
        raster_colorizer_ptr colorizer = sym.get_colorizer();
        if (colorizer)
            colorizer->colorize(raster);
        
        box2d<double> ext=t_.forward(raster->ext_);
        
        int start_x = rint(ext.minx());
        int start_y = rint(ext.miny());
        int raster_width = rint(ext.width());
        int raster_height = rint(ext.height());
        int end_x = start_x + raster_width;
        int end_y = start_y + raster_height;
        double err_offs_x = (ext.minx()-start_x + ext.maxx()-end_x)/2;
        double err_offs_y = (ext.miny()-start_y + ext.maxy()-end_y)/2;
        
        if ( raster_width > 0 && raster_height > 0)
        {
            image_data_32 target(raster_width,raster_height);
          
            if (sym.get_scaling() == "fast") {
                scale_image<image_data_32>(target,raster->data_);
            } else if (sym.get_scaling() == "bilinear"){
                scale_image_bilinear<image_data_32>(target,raster->data_, err_offs_x, err_offs_y);
            } else if (sym.get_scaling() == "bilinear8"){
                scale_image_bilinear8<image_data_32>(target,raster->data_, err_offs_x, err_offs_y);
            } else {
                scale_image<image_data_32>(target,raster->data_);
            }
            
            if (sym.get_mode() == "normal"){
                if (sym.get_opacity() == 1.0) {
                    pixmap_.set_rectangle(start_x,start_y,target);
                } else {
                    pixmap_.set_rectangle_alpha2(target,start_x,start_y, sym.get_opacity());
                }
            } else if (sym.get_mode() == "grain_merge"){
                pixmap_.template merge_rectangle<MergeGrain> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "grain_merge2"){
                pixmap_.template merge_rectangle<MergeGrain2> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "multiply"){
                pixmap_.template merge_rectangle<Multiply> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "multiply2"){
                pixmap_.template merge_rectangle<Multiply2> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "divide"){
                pixmap_.template merge_rectangle<Divide> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "divide2"){
                pixmap_.template merge_rectangle<Divide2> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "screen"){
                pixmap_.template merge_rectangle<Screen> (target,start_x,start_y, sym.get_opacity());
            } else if (sym.get_mode() == "hard_light"){
                pixmap_.template merge_rectangle<HardLight> (target,start_x,start_y, sym.get_opacity());
            } else {
                if (sym.get_opacity() == 1.0){
                    pixmap_.set_rectangle(start_x,start_y,target);
                } else {
                    pixmap_.set_rectangle_alpha2(target,start_x,start_y, sym.get_opacity());
                }
            }
            // TODO: other modes? (add,diff,sub,...)
        }
    }
}


template class agg_renderer<image_32>;
}
