// Copyright (c) 2018 The Blocknet Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blocknettools.h"
#include "blocknetpeerslist.h"
#include "blocknetbip38tool.h"
#include "blocknetdebugconsole.h"
#include "blocknetwalletrepair.h"

#include <QTimer>
#include <QEvent>
#include <QMessageBox>
#include <QList>
#include <QSettings>

enum BToolsTabs {
    DEBUG_CONSOLE = 1,
    NETWORK_MONITOR,
    PEERS_LIST,
    BIP38_TOOL,
    WALLET_REPAIR,
    MULTISEND,
};

BlocknetToolsPage::BlocknetToolsPage(int id, QFrame *parent) : QFrame(parent), pageID(id) { }

BlocknetTools::BlocknetTools(QFrame *parent) : QFrame(parent), layout(new QVBoxLayout) {
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->setContentsMargins(46, 10, 50, 0);
    this->setLayout(layout);

    titleLbl = new QLabel(tr("Tools"));
    titleLbl->setObjectName("h4");
    titleLbl->setFixedHeight(26);

    debugConsole = new BlocknetDebugConsole(this, DEBUG_CONSOLE);
//    networkMonitor = new BlocknetPeersList(this, NETWORK_MONITOR);
    peersList = new BlocknetPeersList(this, PEERS_LIST);
//    bip38Tool = new BlocknetBIP38Tool(this, BIP38_TOOL);
    walletRepair = new BlocknetWalletRepair(this, WALLET_REPAIR);
//    multisend = new BlocknetPeersList(this, MULTISEND);
    pages = { debugConsole, /*networkMonitor,*/ peersList, /*bip38Tool,*/ walletRepair/*, multisend*/ };

    tabBar = new BlocknetTabBar;
    tabBar->setParent(this);
    tabBar->addTab(tr("Debug Console"), DEBUG_CONSOLE);
//    tabBar->addTab(tr("Network Monitor"), NETWORK_MONITOR); // TODO Network monitor
    tabBar->addTab(tr("Peers List"), PEERS_LIST);
//    tabBar->addTab(tr("BIP38 Tool"), BIP38_TOOL);
    tabBar->addTab(tr("Wallet Repair"), WALLET_REPAIR);
//    tabBar->addTab(tr("Multisend"), MULTISEND); // TODO Multisend
    tabBar->show();

    connect(tabBar, SIGNAL(tabChanged(int)), this, SLOT(tabChanged(int)));
    connect(walletRepair, &BlocknetWalletRepair::handleRestart, this, [this](QStringList args) {
        emit handleRestart(args);
    });

    layout->addWidget(titleLbl);
    layout->addWidget(tabBar);

    QSettings settings;
    tabChanged(settings.value("nToolsTab", DEBUG_CONSOLE).toInt());
}

void BlocknetTools::setModels(WalletModel *w, ClientModel *c) {
    if (walletModel == w && clientModel == c)
        return;
    walletModel = w;
    clientModel = c;

    debugConsole->setWalletModel(walletModel);
//    networkMonitor->setWalletModel(walletModel);
    peersList->setClientModel(clientModel);
//    bip38Tool->setWalletModel(walletModel);
    walletRepair->setWalletModel(walletModel);
//    multisend->setWalletModel(walletModel);
}

void BlocknetTools::focusInEvent(QFocusEvent *evt) {
    QWidget::focusInEvent(evt);
    if (screen)
        screen->setFocus();
}

void BlocknetTools::tabChanged(int tab) {
    tabBar->showTab(tab);
    QSettings settings;
    settings.setValue("nToolsTab", tab);

    if (screen) {
        layout->removeWidget(screen);
        screen->hide();
    }

    switch(tab) {
        case DEBUG_CONSOLE:
            screen = debugConsole;
            break;
//        case NETWORK_MONITOR:
//            screen = networkMonitor;
//            break;
        case PEERS_LIST:
            screen = peersList;
            break;
//        case BIP38_TOOL:
//            screen = bip38Tool;
//            break;
        case WALLET_REPAIR:
            screen = walletRepair;
            break;
//        case MULTISEND:
//            screen = multisend;
//            break;
        default:
            return;
    }

    layout->addWidget(screen);

    screen->show();
    screen->setFocus();
}

