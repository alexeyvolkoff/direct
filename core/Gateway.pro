TEMPLATE = subdirs
CONFIG += force_qmake

# SVAR is defined in Node/server.pro: qmake does not propagate DEFINES
# of a subdirs project to its subprojects.
DEFINES += SVAR

SUBDIRS += MasterNode
MasterNode.file = Node/server.pro
