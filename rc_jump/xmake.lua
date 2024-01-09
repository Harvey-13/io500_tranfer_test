add_rules("mode.debug", "mode.release")
add_links("pthread", "rdmacm", "ibverbs")
set_languages("cxx20")

target("server")
    set_kind("binary")
    add_files("server.cc", "../histogram/*.cc")
    

target("client_rw")
    set_kind("binary")
    add_files("client_rw.cc")

target("forwarder")
    set_kind("binary")
    add_files("forwarder.cc")