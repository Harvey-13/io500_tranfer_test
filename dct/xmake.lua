add_rules("mode.debug", "mode.release")
add_links("pthread", "rdmacm", "ibverbs", "mlx5")
set_languages("cxx20")

target("server")
    set_kind("binary")
    add_files("server.cc", "../histogram/*.cc")
    

target("client")
    set_kind("binary")
    add_files("client.cc")

target("query")
    set_kind("binary")
    add_files("query_dct.cc")