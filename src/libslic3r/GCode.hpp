#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "libslic3r.h"
#include "EdgeGrid.hpp"
#include "ExPolygon.hpp"
#include "GCodeWriter.hpp"
#include "Layer.hpp"
#include "MotionPlanner.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/SeamPlacer.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "GCode/ThumbnailData.hpp"

#include <memory>
#include <string>

#ifdef HAS_PRESSURE_EQUALIZER
#include "GCode/PressureEqualizer.hpp"
#endif /* HAS_PRESSURE_EQUALIZER */

namespace Slic3r {

// Forward declarations.
class GCode;

namespace { struct Item; }
struct PrintInstance;
using PrintObjectPtrs = std::vector<PrintObject*>;

class AvoidCrossingPerimeters {
public:
    
    // this flag triggers the use of the external configuration space
    bool use_external_mp;
    bool use_external_mp_once;  // just for the next travel move
    
    // this flag disables avoid_crossing_perimeters just for the next travel move
    // we enable it by default for the first travel move in print
    bool disable_once;
    
    AvoidCrossingPerimeters() : use_external_mp(false), use_external_mp_once(false), disable_once(true) {}
    ~AvoidCrossingPerimeters() {}

    void reset() { m_external_mp.reset(); m_layer_mp.reset(); }
	void init_external_mp(const Print &print);
    void init_layer_mp(const ExPolygons &islands) { m_layer_mp = Slic3r::make_unique<MotionPlanner>(islands); }

    Polyline travel_to(const GCode &gcodegen, const Point &point);

    bool is_init() { return (use_external_mp || use_external_mp_once) ? m_external_mp.get() != nullptr : m_layer_mp.get() != nullptr; }
private:
    // For initializing the regions to avoid.
	static Polygons collect_contours_all_layers(const PrintObjectPtrs& objects);

    std::unique_ptr<MotionPlanner> m_external_mp;
    std::unique_ptr<MotionPlanner> m_layer_mp;
};


class OozePrevention {
public:
    bool enable;
    Points standby_points;
    
    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);
    
private:
    int _get_temp(GCode &gcodegen);
};

class Wipe {
public:
    bool enable;
    Polyline path;
    
    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, bool toolchange = false);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const WipeTower::ToolChangeResult                           &final_purge) :
        m_left(/*float(print_config.wipe_tower_x.value)*/ 0.f),
        m_right(float(/*print_config.wipe_tower_x.value +*/ print_config.wipe_tower_width.value)),
        m_wipe_tower_pos(float(print_config.wipe_tower_x.value), float(print_config.wipe_tower_y.value)),
        m_wipe_tower_rotation(float(print_config.wipe_tower_rotation_angle)),
        m_extruder_offsets(print_config.extruder_offset.values),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_brim_done(false) {}

    std::string prime(GCode &gcodegen);
    void next_layer() { ++ m_layer_idx; m_tool_change_idx = 0; }
    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);
    std::vector<float> used_filament_length() const;

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;

    // Postprocesses gcode: rotates and moves G1 extrusions and returns result
    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const;

    // Left / right edges of the wipe tower, for the planning of wipe moves.
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    const std::vector<Vec2d>                                     m_extruder_offsets;

    // Reference to cached values at the Printer class.
    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    bool                                                         m_brim_done;
    bool                                                         i_have_brim = false;
    double                                                       m_last_wipe_tower_print_z = 0.f;
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

class GCode : ExtrusionVisitorConst  {
public:        
    GCode() : 
    	m_origin(Vec2d::Zero()),
        m_enable_loop_clipping(true), 
        m_enable_cooling_markers(false), 
        m_enable_extrusion_role_markers(false), 
        m_last_processor_extrusion_role(erNone),
        m_layer_count(0),
        m_layer_index(-1), 
        m_layer(nullptr), 
        m_volumetric_speed(0),
        m_last_pos_defined(false),
        m_last_extrusion_role(erNone),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_last_mm3_per_mm(0.0),
        m_last_width(0.0f),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_silent_time_estimator_enabled(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max())),
        m_last_too_small(ExtrusionRole::erNone)
        {}
    ~GCode() {}

    // throws std::runtime_exception on error,
    // throws CanceledException through print->throw_if_canceled().
    void            do_export(Print* print, const char* path, GCodeProcessor::Result* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    const Point&    last_pos() const { return m_last_pos; }
    Vec2d           point_to_gcode(const Point &point) const;
    Vec3d           point_to_gcode(const Point &point, coord_t z_offset) const;
    Point           gcode_to_point(const Vec2d &point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser; }
    // Process a template through the placeholder parser, collect error messages to be reported
    // inside the generated string and after the G-code export finishes.
    std::string     placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }
    std::string     extrusion_role_to_string_for_parser(const ExtrusionRole &);

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_config(const PrintConfig &print_config);

    // append full config to the given string
    static void append_full_config(const Print& print, std::string& str);

    // called by porcess_layer, do the color change / custom gcode
    std::string emit_custom_gcode_per_print_z(const CustomGCode::Item* custom_gcode, unsigned int first_extruder_id, const Print& print, PrintStatistics& stats);

    // Object and support extrusions of the same PrintObject at the same print_z.
    // public, so that it could be accessed by free helper functions from GCode.cpp
    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr) {}
        const Layer* 		object_layer;
        const SupportLayer* support_layer;
        const Layer* 		layer()   const { return (object_layer != nullptr) ? object_layer : support_layer; }
        const PrintObject* 	object()  const { return (this->layer() != nullptr) ? this->layer()->object() : nullptr; }
        coordf_t            print_z() const { return (object_layer != nullptr && support_layer != nullptr) ? 0.5 * (object_layer->print_z + support_layer->print_z) : this->layer()->print_z; }
    };

private:
    void            _do_export(Print &print, FILE *file, ThumbnailsGeneratorCallback thumbnail_cb);

    static std::vector<LayerToPrint>        		                   collect_layers_to_print(const PrintObject &object);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);
    void            process_layer(
        // Write into the output file.
        FILE                            *file,
        const Print                     &print,
        PrintStatistics                 &print_stat,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const std::vector<LayerToPrint> &layers,
        const LayerTools  				&layer_tools,
        // Pairs of PrintObject index and its instance index.
        const std::vector<const PrintInstance*> *ordering,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        size_t                     single_object_idx = size_t(-1)
        );

    void            set_last_pos(const Point &pos) { m_last_pos = pos; m_last_pos_defined = true; }
    bool            last_pos_defined() const { return m_last_pos_defined; }
    void            set_extruders(const std::vector<uint16_t> &extruder_ids);
    std::string     preamble();
    std::string     change_layer(coordf_t print_z);
    std::string     visitor_gcode;
    std::string     visitor_comment;
    double          visitor_speed;
    std::unique_ptr<EdgeGrid::Grid> *visitor_lower_layer_edge_grid;
    virtual void use(const ExtrusionPath &path) override { visitor_gcode += extrude_path(path, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionPath3D &path3D) override { visitor_gcode += extrude_path_3D(path3D, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionMultiPath &multipath) override { visitor_gcode += extrude_multi_path(multipath, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionMultiPath3D &multipath) override { visitor_gcode += extrude_multi_path3D(multipath, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionLoop &loop) override { visitor_gcode += extrude_loop(loop, visitor_comment, visitor_speed, visitor_lower_layer_edge_grid); };
    virtual void use(const ExtrusionEntityCollection &collection) override;
    std::string     extrude_entity(const ExtrusionEntity &entity, const std::string &description, double speed = -1., std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid = nullptr);
    std::string     extrude_loop(const ExtrusionLoop &loop, const std::string &description, double speed = -1., std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid = nullptr);
    std::string     extrude_loop_vase(const ExtrusionLoop &loop, const std::string &description, double speed = -1., std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid = nullptr);
    std::string     extrude_multi_path(const ExtrusionMultiPath &multipath, const std::string &description, double speed = -1.);
    std::string     extrude_multi_path3D(const ExtrusionMultiPath3D &multipath, const std::string &description, double speed = -1.);
    std::string     extrude_path(const ExtrusionPath &path, const std::string &description, double speed = -1.);
    std::string     extrude_path_3D(const ExtrusionPath3D &path, const std::string &description, double speed = -1.);
    void            split_at_seam_pos(ExtrusionLoop &loop, std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid, bool was_clockwise);

    // Extruding multiple objects with soluble / non-soluble / combined supports
    // on a multi-material printer, trying to minimize tool switches.
    // Following structures sort extrusions by the extruder ID, by an order of objects and object islands.
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        // erSupportMaterial / erSupportMaterialInterface or erMixed.
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
            	// Non-owned references to LayerRegion::perimeters::entities
            	// std::vector<const ExtrusionEntity*> would be better here, but there is no way in C++ to convert from std::vector<T*> std::vector<const T*> without copying.
                ExtrusionEntitiesPtr perimeters;
            	// Non-owned references to LayerRegion::fills::entities
                ExtrusionEntitiesPtr infills;
                // Non-owned references to LayerRegion::ironing::entities
                ExtrusionEntitiesPtr ironings;

                std::vector<const WipingExtrusions::ExtruderPerCopy*> infills_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> perimeters_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> ironings_overrides;

	            enum Type {
	            	PERIMETERS,
                    INFILL,
                    IRONING,
	            };

                // Appends perimeter/infill entities and writes don't indices of those that are not to be extruder as part of perimeter/infill wiping
                void append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copy_extruders);
            };


            std::vector<Region> by_region;                                    // all extrusions for this island, grouped by regions

            // Fills in by_region_per_copy_cache and returns its reference.
            const std::vector<Region>& by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities = false) const;
        };
        std::vector<Island>         islands;
    };

	struct InstanceToPrint
	{
		InstanceToPrint(ObjectByExtruder &object_by_extruder, size_t layer_id, const PrintObject &print_object, size_t instance_id) :
			object_by_extruder(object_by_extruder), layer_id(layer_id), print_object(print_object), instance_id(instance_id) {}

		// Repository 
		ObjectByExtruder		&object_by_extruder;
		// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const size_t       		 layer_id;
		const PrintObject 		&print_object;
		// Instance idx of the copy of a print object.
		const size_t			 instance_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		std::vector<ObjectByExtruder> 					&objects_by_extruder,
		// Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const std::vector<LayerToPrint> 				&layers,
		// Ordering must be defined for normal (non-sequential print).
		const std::vector<const PrintInstance*>     	*ordering,
		// For sequential print, the instance of the object to be printing has to be defined.
		const size_t                     				 single_object_instance_idx);

    std::string     extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, std::unique_ptr<EdgeGrid::Grid> &lower_layer_edge_grid);
    std::string     extrude_infill(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool is_infill_first);
    std::string     extrude_ironing(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region);
    std::string     extrude_support(const ExtrusionEntityCollection &support_fills);

    std::string     travel_to(const Point &point, ExtrusionRole role, std::string comment);
    bool            needs_retraction(const Polyline &travel, ExtrusionRole role = erNone);
    std::string     retract(bool toolchange = false);
    std::string     unretract() { return m_writer.unlift() + m_writer.unretract(); }
    std::string     set_extruder(unsigned int extruder_id, double print_z, bool no_toolchange = false);

    // Cache for custom seam enforcers/blockers for each layer.
    SeamPlacer                          m_seam_placer;

    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    GCodeWriter                         m_writer;
    PlaceholderParser                   m_placeholder_parser;
    // Collection of templates, on which the placeholder substitution failed.
    std::set<std::string>               m_placeholder_parser_failed_templates;
    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    bool                                m_enable_loop_clipping;
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _BRIDGE_FAN_START, _BRIDGE_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // Keeps track of the last extrusion role passed to the processor
    ExtrusionRole                       m_last_processor_extrusion_role;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    unsigned int                        m_layer_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. Insequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    ExtrusionRole                       m_last_extrusion_role;
    // Support for G-Code Processor
    float                               m_last_height{ 0.0f };
    float                               m_last_layer_z{ 0.0f };
    float                               m_max_layer_z{ 0.0f };
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    double                              m_last_mm3_per_mm;
    float                               m_last_width{ 0.0f };
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    Point                               m_last_pos;
    bool                                m_last_pos_defined;

    // a previous extrusion path that is too small to be extruded, have to fusion it into the next call.
    ExtrusionPath                       m_last_too_small;

    std::unique_ptr<CoolingBuffer>      m_cooling_buffer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;
#ifdef HAS_PRESSURE_EQUALIZER
    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
#endif /* HAS_PRESSURE_EQUALIZER */
    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;

    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>               m_skirt_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // Index of a last object copy extruded.
    std::pair<const PrintObject*, Point> m_last_obj_copy;

    // ordered list of object, to give them a unique id.
    std::vector<const PrintObject*> m_ordered_objects;
    // gcode for the start/end of the current object block.
    // as the retraction/unretraction can be written after the start/end of the algoruihtmblock, it has to be delayed.
    std::string m_gcode_label_objects_start;
    std::string m_gcode_label_objects_end;
    void _add_object_change_labels(std::string &gcode);

    bool m_silent_time_estimator_enabled;

    // Processor
    GCodeProcessor m_processor;

    // Write a string into a file.
    void _write(FILE* file, const std::string& what, bool flush = false) { this->_write(file, what.c_str(), flush); }
    void _write(FILE* file, const char *what, bool flush = false);

    // Write a string into a file. 
    // Add a newline, if the string does not end with a newline already.
    // Used to export a custom G-code section processed by the PlaceholderParser.
    void _writeln(FILE* file, const std::string& what);

    // Formats and write into a file the given data. 
    void _write_format(FILE* file, const char* format, ...);

    //some post-processing on the file, with their data class
    std::unique_ptr<FanMover> m_fan_mover;
    void _post_process(std::string& what, bool flush = true);

    std::string _extrude(const ExtrusionPath &path, const std::string &description, double speed = -1);
    std::string _before_extrude(const ExtrusionPath &path, const std::string &description, double speed = -1);
    std::string _after_extrude(const ExtrusionPath &path);
    void print_machine_envelope(FILE *file, Print &print);
    void _print_first_layer_bed_temperature(FILE *file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    void _print_first_layer_extruder_temperatures(FILE *file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    // this flag triggers first layer speeds
    bool                                on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0; }

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder, 
        unsigned int                                           extruder_id, 
        size_t                                                 object_idx, 
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder, 
        unsigned int                                            extruder_id, 
        size_t                                                  object_idx, 
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    friend class Wipe;
    friend class WipeTowerIntegration;
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print);

}

#endif
