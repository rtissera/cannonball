/***************************************************************************
    Core Game Engine Routines.
    
    - The main loop which advances the level onto the next segment.
    - Code to directly control the road hardware. For example, the road
      split and bonus points routines.
    - Code to determine whether to initialize certain game modes
      (Crash state, Bonus points, road split state) 
    
    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include "engine/outils.hpp"
#include "engine/opalette.hpp"
#include "engine/oinitengine.hpp"

OInitEngine oinitengine;

OInitEngine::OInitEngine()
{
}


OInitEngine::~OInitEngine()
{
}

// Source: 0x8360
void OInitEngine::init()
{
    ostats.game_completed = 0;

    // Clear shared ram area (0x260000 - 0x267FFF). Unfinished.
    ingame_counter = 0;
    ostats.cur_stage = 0;
    oroad.stage_lookup_off = 0;
    rd_split_state = SPLIT_NONE;
    car_x_pos = 0;
    car_x_old = 0;
    road_curve = 0;
    road_curve_next = 0;
    road_remove_split = 0;
    road_width_next = 0;
    road_width_adj = 0;
    change_width = 0;
    granular_rem = 0;
    pos_fine_old = 0;
    road_width_orig = 0;
    road_width_merge = 0;
    route_updated = 0;

    // todo: Note we need to include code to skip SetSoundReset when initgameengine2 is called
	init_road_seg_master();
	opalette.setup_sky_palette();
	opalette.setup_ground_color();
	opalette.setup_road_centre();
	opalette.setup_road_stripes();
	opalette.setup_road_side();
	opalette.setup_road_colour();   
    otiles.setup_palette_default();                 // Init Default Palette
    osprites.copy_palette_data();                   // Copy Palette Data to RAM
    otiles.setup_palette_tilemap();                 // Setup Palette For Tilemap
    setup_stage1();                                 // Setup Misc stuff relating to Stage 1
    otiles.reset_tiles_pal();                       // Reset Tiles, Palette And Road Split Data

    ocrash.clear_crash_state();
    ocrash.skid_counter = 0;

    osoundint.reset();
}

void OInitEngine::debug_load_level(uint8_t level)
{
    ostats.cur_stage = level / 8;
    oroad.stage_lookup_off = level;
    setup_stage1();
    init_road_seg_master();

    // Road Renderer: Setup correct stage address 
	oroad.stage_addr = roms.rom1.read32(ROAD_DATA_LOOKUP + (stage_data[oroad.stage_lookup_off] << 2));

    opalette.setup_sky_palette();
    opalette.setup_ground_color();
	opalette.setup_road_centre();
	opalette.setup_road_stripes();
	opalette.setup_road_side();
	opalette.setup_road_colour();
    otiles.setup_palette_default();                 // Init Default Palette
    osprites.copy_palette_data();                   // Copy Palette Data to RAM
    otiles.setup_palette_tilemap();                 // Setup Palette For Tilemap
    otiles.reset_tiles_pal();    
    ocrash.clear_crash_state();
    
    // Reset Tiles, Palette And Road Split Data
    otiles.init_tilemap_props(level);
    otiles.copy_fg_tiles(ostats.cur_stage & 1 ? 0x104F80 : 0x100F80);
    otiles.copy_bg_tiles(ostats.cur_stage & 1 ? 0x10BF80 : 0x108F80);
    otiles.tilemap_ctrl = OTiles::TILEMAP_SCROLL;    
    otiles.init_tilemap_palette(level);

    oroad.road_width = RD_WIDTH_MERGE << 16;        // Setup a default road width

    // Hacks
    outrun.game_state = 0xC;
    oferrari.reset_car();
    car_x_pos = 0;
}

// Source: 0x8402
void OInitEngine::setup_stage1()
{
    oroad.road_width = 0x1C2 << 16;     // Force display of two roads at start
    ostats.score = 0;
    ostats.clear_stage_times();
    oferrari.reset_car();               // Reset Car Speed/Rev Values
    osoundint.engine_data[sound::ENGINE_VOL] = 0x3F;
    ostats.extend_play_timer = 0;
    checkpoint_marker = 0;              // Denote not past checkpoint marker
    otraffic.set_max_traffic();         // Set Number Of Enemy Cars Based On Dip Switches
    ostats.clear_route_info();
    osmoke.setup_smoke_sprite(true);
}

// Initialise Master Segment Address For Stage
//
// 1. Read Internal Stage Number from Stage Data Table (Using the lookup offset)
// 2. Load the master address, using the stage number as an index.
//
// Source: 0x8C80

void OInitEngine::init_road_seg_master()
{
    uint16_t stage_offset = stage_data[oroad.stage_lookup_off] << 2; // convert to long
    road_seg_master = roms.rom0.read32(ROAD_SEG_TABLE + stage_offset);

    // Rolled the following lines in from elsewhere
	road_seg_addr3 = roms.rom0.read32(0x18 + road_seg_master); // Type of curve and unknown
	road_seg_addr2 = roms.rom0.read32(0x1C + road_seg_master); // Width/Height Lookup
	road_seg_addr1 = roms.rom0.read32(0x20 + road_seg_master); // Sprite information
}

//
// Check Road Width
// Source: B85A
//
// Potentially Update Width Of Road
//
// ADDRESS 2 - Road Segment Data [8 byte boundaries]:
//
// Word 1 [+0]: Segment Position
// Word 2 [+2]: Set = Denotes Road Height Info. Unset = Denotes Road Width
// Word 3 [+4]: Segment Road Width / Segment Road Height Index
// Word 4 [+6]: Segment Width Adjustment SIGNED (Speed at which width is adjusted)

void OInitEngine::check_road_width()
{
    check_road_split(); // Check/Process road split if necessary
    uint32_t addr = road_seg_addr2;
    uint16_t d0 = roms.rom0.read16(&addr);
    
    // Update next road section
    if (d0 <= oroad.road_pos >> 16)
    {
        // Skip road width adjustment if set and adjust height
        if (roms.rom0.read16(&addr) == 0)
        {
            // ROM:0000B8A6 skip_next_width
            if (oroad.height_lookup == 0)
                oroad.height_lookup = roms.rom0.read16(addr); // Set new height lookup section
        }
        else
        {
            // ROM:0000B87A
            int16_t d0 = roms.rom0.read16(&addr); // Segment road width
            int16_t d1 = roms.rom0.read16(&addr); // Segment adjustment speed

            if (d0 != (int16_t) (oroad.road_width >> 16))
            {
                if (d0 <= (int16_t) (oroad.road_width >> 16))
                    d1 = -d1;

                road_width_next = d0;
                road_width_adj = d1;
                change_width = -1; // Denote road width is changing
            }
        }
        road_seg_addr2 += 8;
    }

    // ROM:0000B8BC set_road_width    
    // Width of road is changing & car is moving
    if (change_width != 0 && car_increment >> 16 != 0)
    {
        int32_t d0 = ((car_increment >> 16) * road_width_adj) << 4;
        oroad.road_width += d0; // add long here
        if (d0 > 0)
        {    
            if (road_width_next < (int16_t) (oroad.road_width >> 16))
            {
                change_width = 0;
                oroad.road_width = road_width_next << 16;
            }
        }
        else
        {
            if (road_width_next >= (int16_t) (oroad.road_width >> 16))
            {
                change_width = 0;
                oroad.road_width = road_width_next << 16;
            }
        }
    }
    // ------------------------------------------------------------------------------------------------
    // ROAD SEGMENT FORMAT
    //
    // Each segment of road is 6 bytes in memory, consisting of 3 words
    // Each road segment is a signifcant length of road btw :)
    //
    // ADDRESS 3 - Road Segment Data [6 byte boundaries]
    //
    // Word 1 [+0]: Segment Position (used with 0x260006 car position)
    // Word 2 [+2]: Segment Road Curve
    // Word 3 [+4]: Segment Road type (1 = Straight, 2 = Right Bend, 3 = Left Bend)
    //
    // 60a08 = address of next road segment? (e.g. A0 = 0x0001DD86)
    // ------------------------------------------------------------------------------------------------

    // ROM:0000B91C set_road_type: 

    int16_t segment_pos = roms.rom0.read16(road_seg_addr3);

    if (segment_pos != -1)
    {
        int16_t d1 = segment_pos - 0x3C;

        if (d1 <= (int16_t) (oroad.road_pos >> 16))
        {
            road_curve_next = roms.rom0.read16(2 + road_seg_addr3);
            road_type_next  = roms.rom0.read16(4 + road_seg_addr3);
        }

        if (segment_pos <= (int16_t) (oroad.road_pos >> 16))
        {
            road_curve = roms.rom0.read16(2 + road_seg_addr3);
            road_type  = roms.rom0.read16(4 + road_seg_addr3);
            road_seg_addr3 += 6;
            road_type_next = 0;
            road_curve_next = 0;
        }
    }
    
    // ------------------------------------------------------------------------
    // TILE MAP OFFSETS
    // ROM:0000B986 setup_shadow_offset:
    // Setup the shadow offset based on how much we've scrolled left/right. Lovely and subtle!
    // ------------------------------------------------------------------------

    int16_t shadow_off = oroad.tilemap_h_target & 0x3FF;
    if (shadow_off > 0x1FF)
        shadow_off = -shadow_off + 0x3FF;
    shadow_off >>= 2;
    if (oroad.tilemap_h_target & BIT_A)
        shadow_off = -shadow_off; // reverse direction of shadow
    osprites.shadow_offset = shadow_off;

    // ------------------------------------------------------------------------
    // Main Car Logic Block
    // ------------------------------------------------------------------------

    if (DEBUG_LEVEL)
    {
        uint32_t result = 0x12F * (car_increment >> 16);
        oroad.road_pos_change = result;
        oroad.road_pos += result;
    }
    else
    {
        oferrari.move();

        if (oferrari.car_ctrl_active)
        {
            oferrari.set_curve_adjust();
            oferrari.set_ferrari_x();
            oferrari.do_skid();
            oferrari.check_wheels();
            oferrari.set_ferrari_bounds();
        }

        oferrari.do_sound_score_slip();
    }

    // ------------------------------------------------------------------------
    // Setup New Sprite Scroll Speed. Based On Granular Difference.
    // ------------------------------------------------------------------------
    set_granular_position();

    d0 = oroad.pos_fine - pos_fine_old;
    if (d0 > 0xF)
        d0 = 0xF;

    d0 <<= 0xB;
    osprites.sprite_scroll_speed = d0;

    pos_fine_old = oroad.pos_fine;

    // Draw Speed & Hud Stuff
    if (outrun.game_state >= GS_START1 && outrun.game_state <= GS_BONUS)
    {
        // Convert & Blit Car Speed
        ohud.blit_speed(0x110CB6, car_increment >> 16);
        ohud.blit_text1(HUD_KPH1);
        ohud.blit_text1(HUD_KPH2);

        // Blit High/Low Gear
        if (oinputs.gear)
            ohud.blit_text_new(9, 26, "H");
        else
            ohud.blit_text_new(9, 26, "L");
    }

    if (olevelobjs.spray_counter > 0)
        olevelobjs.spray_counter--;

    if (olevelobjs.sprite_collision_counter > 0)
        olevelobjs.sprite_collision_counter--;

    opalette.setup_sky_cycle();
}

// Check for Road Split
//
// - Checks position in level and determine whether to init road split
// - Processes road split if initialized
//
// Source: 868E
void OInitEngine::check_road_split()
{
    // Check whether to initialize the next level
    ostats.init_next_level();

    switch (rd_split_state)
    {
        // State 0: No road split. Check current road position with 0x79C.
        case SPLIT_NONE:
            if (oroad.road_pos >> 16 <= 0x79C) return; 
            check_stage(); // Do Split - Split behaviour depends on stage
            break;

        // State 1: (Does this ever need to be called directly?)
        case SPLIT_INIT:
            init_split1();
            break;
        
        // State 2: Road Split
        case SPLIT_CHOICE1:
            if (oroad.road_pos >> 16 >= 0x3F)  
                init_split2();            
            break;

        // State 3: Beginning of split. User must choose.
        case SPLIT_CHOICE2:
            init_split2();
            break;

        // State 4: Road physically splits into two individual roads
        case 4:
            init_split3();
            break;

        // State 5: Road fully split. Init remove other road
        case 5:
            if (!road_curve)
                rd_split_state = 6; // and fall through
            else
                break;
        
        // State 6: Road split. Only one road drawn.
        case 6:
            init_split5();
            break;

        // Stage 7
        case 7:
            init_split6();
            break;

        // State 8: Init Road Merge before checkpoint sign
        case 8:
            otraffic.traffic_split = -1;
            rd_split_state = 9;
            break;

        // State 9: Road Merge before checkpoint sign
        case 9:
            otraffic.traffic_split = 0;
        case 0x0A:
            init_split9();
            break;

        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
            init_split10();
            break;
        
        // Init Bonus Sequence
        case 0x10:
            init_bonus();
            break;

        case 0x11:
            bonus1();
            break;

        case 0x12:
            bonus2();
            break;

        case 0x13:
            bonus3();
            break;

        case 0x14:
            bonus4();
            break;

        case 0x15:
            bonus5();
            break;

        case 0x16:
        case 0x17:
        case 0x18:
            bonus6();
            break;
    }
}

// ------------------------------------------------------------------------------------------------
// Check Stage Info To Determine What To Do With Road
//
// Stage 1-4: Init Road Split
// Stage 5: Init Bonus
// Stage 5 ATTRACT: Loop to Stage 1
// ------------------------------------------------------------------------------------------------
void OInitEngine::check_stage()
{
    // Stages 0-4, do road split
    if (ostats.cur_stage <= 3)
    {
        rd_split_state = SPLIT_INIT;
        init_split1();
    }
    // Stage 5: Init Bonus
    else if (outrun.game_state == GS_INGAME)
    {
        init_bonus();
    }
    // Stage 5 Attract Mode: Reload Stage 1
    else
    {
        oroad.road_pos = 0;
        oroad.tilemap_h_target = 0;
        ostats.cur_stage = -1;
        oroad.stage_lookup_off = -8;

        ostats.clear_route_info();

        end_stage_props |= BIT_1; // Loop back to stage 1 (Used by tilemap code)
        end_stage_props |= BIT_2;
        end_stage_props |= BIT_3;
        osmoke.setup_smoke_sprite(true);
        init_split_next_level();
    }
}

// ------------------------------------------------------------------------------------------------
// Road Split 1
// Init Road Split & Begin Road Split
// Called When We're Not On The Final Stage
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split1()
{
    rd_split_state = SPLIT_CHOICE1;

    oroad.road_load_split = -1;
    oroad.road_ctrl = ORoad::ROAD_BOTH_P0_INV; // Both Roads (Road 0 Priority) (Road Split. Invert Road 0)
    road_width_orig = oroad.road_width >> 16;
    oroad.road_pos = 0;
    oroad.tilemap_h_target = 0;
    road_seg_addr3 = roms.rom0.read32(ROAD_DATA_SPLIT_SEGS);
    road_seg_addr2 = roms.rom0.read32(ROAD_DATA_SPLIT_SEGS + 4);
    road_seg_addr1 = roms.rom0.read32(ROAD_DATA_SPLIT_SEGS + 8);
}

// ------------------------------------------------------------------------------------------------
// Road Split 2: Beginning of split. User must choose.
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split2()
{
    rd_split_state = SPLIT_CHOICE2;

    // Manual adjustments to the road width, based on the current position
    int16_t pos = (((oroad.road_pos >> 16) - 0x3F) << 3) + road_width_orig;
    oroad.road_width = (pos << 16) | (oroad.road_width & 0xFFFF);
    if (pos > 0xFF)
    {
        route_updated &= ~BIT_0;
        init_split3();
    }
}

// ------------------------------------------------------------------------------------------------
// Road Split 3: Road physically splits into two individual roads
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split3()
{
    rd_split_state = 4;

    int16_t pos = (((oroad.road_pos >> 16) - 0x3F) << 3) + road_width_orig;
    oroad.road_width = (pos << 16) | (oroad.road_width & 0xFFFF);

    if (route_updated & BIT_0 || pos <= 0x168)
    {
        if (oroad.road_width >> 16 > 0x300)
            init_split4();
        return;
    }

    route_updated |= BIT_0; // Denote route info updated
    route_selected = 0;

    // Go Left
    if (car_x_pos > 0)
    {
        route_selected = -1;
        uint8_t inc = 1 << (3 - ostats.cur_stage);

        // One of the following increment values

        // Stage 1 = +8 (1 << 3 - 0)
        // Stage 2 = +4 (1 << 3 - 1)
        // Stage 3 = +2 (1 << 3 - 2)
        // Stage 4 = +1 (1 << 3 - 3)
        // Stage 5 = Road doesn't split on this stage

        ostats.route_info += inc;
        oroad.stage_lookup_off++;
    }
    // Go Right / Continue

    end_stage_props |= BIT_0;                                 // Set end of stage property (road splitting)
    osmoke.load_smoke_data |= BIT_0;                          // Denote we should load smoke sprite data
    ostats.routes[0]++;                                       // Set upcoming stage number to store route info
    ostats.routes[ostats.routes[0]] = ostats.route_info;      // Store route info for course map screen

    if (oroad.road_width >> 16 > 0x300)
        init_split4();
}

// ------------------------------------------------------------------------------------------------
// Road Split 4: Road Fully Split, Remove Other Road
// ------------------------------------------------------------------------------------------------

void OInitEngine::init_split4()
{
    rd_split_state = 5; // init_split4

     // Set Appropriate Road Control Value, Dependent On Route Chosen
    if (route_selected != 0)
        oroad.road_ctrl = ORoad::ROAD_R0_SPLIT;
    else
        oroad.road_ctrl = ORoad::ROAD_R1_SPLIT;

    // Denote road split has been removed (for enemy traFfic logic)
    road_remove_split |= BIT_0;
       
    if (!road_curve)
        init_split5();
}

// ------------------------------------------------------------------------------------------------
// Road Split 5: Only Draw One Section Of Road - Wait For Final Curve
// ------------------------------------------------------------------------------------------------

void OInitEngine::init_split5()
{
    rd_split_state = 6;
    if (road_curve)
        init_split6();
}

// ------------------------------------------------------------------------------------------------
// Road Split 6 - Car On Final Curve Of Split
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split6()
{
    rd_split_state = 7;
    if (!road_curve)
        init_split7();
}

// ------------------------------------------------------------------------------------------------
// Road Split 7: Init Road Merge Before Checkpoint (From Normal Section Of Road)
// ------------------------------------------------------------------------------------------------

void OInitEngine::init_split7()
{
    rd_split_state = 8;

    oroad.road_ctrl = ORoad::ROAD_BOTH_P0;
    route_selected = ~route_selected; // invert bits
    int16_t width2 = (oroad.road_width >> 16) << 1;
    if (route_selected == 0) 
        width2 = -width2;
    car_x_pos += width2;
    car_x_old += width2;
    road_width_orig = oroad.road_pos >> 16;
    road_width_merge = oroad.road_width >> 19; // (>> 16 and then >> 3)
    road_remove_split &= ~BIT_0; // Denote we're back to normal road handling for enemy traffic logic
    
}

// ------------------------------------------------------------------------------------------------
// Road Split 9 - Do Road Merger. Road Gets Narrower Again.
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split9()
{
    rd_split_state = 10;

    // Calculate narrower road width to merge roads
    uint16_t d0 = (road_width_merge - ((oroad.road_pos >> 16) - road_width_orig)) << 3;

    if (d0 <= RD_WIDTH_MERGE)
    {
        oroad.road_width = (RD_WIDTH_MERGE << 16) | (oroad.road_width & 0xFFFF);
        init_split10();
    }
    else
        oroad.road_width = (d0 << 16) | (oroad.road_width & 0xFFFF);
}


// ------------------------------------------------------------------------------------------------
// Road Split A: Checkpoint Sign
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split10()
{
    rd_split_state = 11;

    if (oroad.road_pos >> 16 > 0x180)
    {
        rd_split_state = 0;
        init_split_next_level();
    }
}

// ------------------------------------------------------------------------------------------------
// Road Split B: Init Next Level
// ------------------------------------------------------------------------------------------------
void OInitEngine::init_split_next_level()
{
    oroad.road_pos = 0;
    oroad.tilemap_h_target = 0;
    ostats.cur_stage++;
    oroad.stage_lookup_off += 8;    // Increment lookup to next block of stages
    ostats.route_info += 0x10;      // Route Info increments by 10 at each stage
    ohud.do_mini_map();
    init_road_seg_master();

    // Clear sprite palette lookup
    if (ostats.cur_stage)
        osprites.clear_palette_data();
}

// ------------------------------------------------------------------------------------------------
// Bonus Road Mode Control
// ------------------------------------------------------------------------------------------------

// Initialize new segment of road data for bonus sequence
// Source: 0x8A04
void OInitEngine::init_bonus()
{
    oroad.road_ctrl = ORoad::ROAD_BOTH_P0_INV;
    oroad.road_pos  = 0;
    oroad.tilemap_h_target = 0;
    oanimseq.end_seq = oroad.stage_lookup_off - 0x20; // Set End Sequence (0 - 4)
    uint32_t adr = roms.rom0.read32(ROAD_SEG_TABLE_END + (oanimseq.end_seq << 2)); // Road Data Addr
    road_seg_addr3 = roms.rom0.read32(&adr);
    road_seg_addr2 = roms.rom0.read32(&adr);
    road_seg_addr1 = roms.rom0.read32(&adr);
    outrun.game_state = GS_INIT_BONUS;
    rd_split_state = 0x11;
    bonus1();
}

void OInitEngine::bonus1()
{
    if (oroad.road_pos >> 16 >= 0x5B)
    {
        otraffic.bonus_lhs = 1; // Force traffic spawn on LHS during bonus mode
        rd_split_state = 0x12;
        bonus2();
    }
}

void OInitEngine::bonus2()
{
    if (oroad.road_pos >> 16 >= 0xB6)
    {
        otraffic.bonus_lhs = 0; // Disable forced traffic spawn
        road_width_orig = oroad.road_width >> 16;
        rd_split_state = 0x13;
        bonus3();
    }
}

// Stretch the road to a wider width. It does this based on the car's current position.
void OInitEngine::bonus3()
{
    // Manual adjustments to the road width, based on the current position
    int16_t pos = (((oroad.road_pos >> 16) - 0xB6) << 3) + road_width_orig;
    oroad.road_width = (pos << 16) | (oroad.road_width & 0xFFFF);
    if (pos > 0xFF)
    {
        route_selected = 0;
        if (car_x_pos > 0)
            route_selected = ~route_selected; // invert bits
        rd_split_state = 0x14;
        bonus4();
    }
}

void OInitEngine::bonus4()
{
    // Manual adjustments to the road width, based on the current position
    int16_t pos = (((oroad.road_pos >> 16) - 0xB6) << 3) + road_width_orig;
    oroad.road_width = (pos << 16) | (oroad.road_width & 0xFFFF);
    if (pos > 0x300)
    {
         // Set Appropriate Road Control Value, Dependent On Route Chosen
        if (route_selected != 0)
            oroad.road_ctrl = ORoad::ROAD_R0_SPLIT;
        else
            oroad.road_ctrl = ORoad::ROAD_R1_SPLIT;

        // Denote road split has been removed (for enemy traFfic logic)
        road_remove_split |= BIT_0;
        rd_split_state = 0x15;
        bonus5();
    }
}

// Check for end of curve. Init next state when ended.
void OInitEngine::bonus5()
{
    if (!road_curve)
    {
        rd_split_state = 0x16;
        bonus6();
    }
}

// This state simply checks for the end of bonus mode
void OInitEngine::bonus6()
{
    if (obonus.bonus_control >= OBonus::BONUS_END)
        rd_split_state = 0;
}

// SetGranularPosition
//
// Source: BD3E
//
// Uses the car increment value to set the granular position.
// The granular position is used to finely scroll the road by CPU 1 and smooth zooming of scenery.
//
// Notes:
// Disable with - bpset bd3e,1,{pc = bd76; g}
void OInitEngine::set_granular_position()
{
    uint16_t car_inc16 = car_increment >> 16;

    uint16_t result = car_inc16 / 0x40;
    uint16_t rem = car_inc16 % 0x40;

    granular_rem += rem;
    // When the overall counter overflows past 0x40, we must carry a 1 to the unsigned divide :)
    if (granular_rem >= 0x40)
    {
        granular_rem -= 0x40;
        result++;
    }
    oroad.pos_fine += result;
}

// Check whether to initalize crash or bonus sequence code
//
// Source: 0x984E
void OInitEngine::init_crash_bonus()
{
    if (outrun.game_state == GS_MUSIC) return;

    if (ocrash.skid_counter > 6 || ocrash.skid_counter < -6)
    {
        // do_skid:
        if (otraffic.collision_traffic == 1)
        {   
            otraffic.collision_traffic = 2;
            uint8_t rnd = outils::random() & otraffic.collision_mask;
            if (rnd == otraffic.collision_mask)
            {
                // Try to launch crash code and perform a spin
                if (ocrash.coll_count1 == ocrash.coll_count2)
                {
                    if (!ocrash.spin_control1)
                    {
                        ocrash.spin_control1 = 1;
                        ocrash.skid_counter_bak = ocrash.skid_counter;
                        //ocrash.enable();
                        test_bonus_mode(true); // 9924 fall through
                        return;
                    }
                    test_bonus_mode(false); // finalise skid               
                    return;
                }
                else
                {
                    test_bonus_mode(true); // test_bonus_mode
                    return;
                }
            }
        }
    }
    else if (ocrash.spin_control2 == 1)
    {
        // 9894
        ocrash.spin_control2 = 2;
        if (ocrash.coll_count1 == ocrash.coll_count2)
        {
            ocrash.enable();
        }
        test_bonus_mode(false); // finalise skid
        return;
    }
    else if (ocrash.spin_control1 == 1)
    {
        // 98c0
        ocrash.spin_control1 = 2;
        ocrash.enable();
        test_bonus_mode(false); // finalise skid
        return;
    }

    // 0x9924: Section Of Code
    if (ocrash.coll_count1 == ocrash.coll_count2) 
    {
        test_bonus_mode(true);  // test_bonus_mode
    }
    else
    {
        ocrash.enable();
        test_bonus_mode(false); // finalise skid
    }
}

// Source: 0x993C
void OInitEngine::test_bonus_mode(bool do_bonus_check)
{
    // Bonus checking code 
    if (do_bonus_check && obonus.bonus_control)
    {
        // Do Bonus Text Display
        if (obonus.bonus_state < 3)
            obonus.do_bonus_text();

        // End Seq Animation Stage #0
        if (obonus.bonus_control == OBonus::BONUS_SEQ0)
            oanimseq.init_end_seq();
    }

   // finalise_skid:
   if (!ocrash.skid_counter)
       otraffic.collision_traffic = 0;
}

// Stage Data
//
// This effectively is the master table that controls the order of the stages.
//
// You can change the stage order by editing this table.
// Bear in mind that the double lanes are hard coded in Stage 1.

const uint8_t OInitEngine::stage_data[] = 
{ 
    0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Stage 1
    0x1E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Stage 2
    0x20, 0x2F, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00,  // Stage 3
    0x2D, 0x35, 0x33, 0x21, 0x00, 0x00, 0x00, 0x00,  // Stage 4
    0x32, 0x23, 0x38, 0x22, 0x26, 0x00, 0x00, 0x00,  // Stage 5
};
