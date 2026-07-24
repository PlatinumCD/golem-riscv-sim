import sst


sst.addGlobalParams(
    "analog_geometry",
    {
        "analog_array_count": 3,
        "analog_array_rows": 100,
        "analog_array_columns": 64,
    },
)

tile0 = sst.Component("tile0", "mittens.tile")
tile0.addGlobalParamSet("analog_geometry")
tile0.addParams(
    {
        "tile_id": 0,
        "analog_backend": "crosssim",
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 12,
        "verbose": 1,
    }
)

tile1 = sst.Component("tile1", "mittens.tile")
tile1.addGlobalParamSet("analog_geometry")
tile1.addParams(
    {
        "tile_id": 1,
        "analog_backend": "crosssim",
        "analog_link_clock": "800MHz",
        "analog_compute_latency_cycles": 20,
        "verbose": 1,
    }
)
