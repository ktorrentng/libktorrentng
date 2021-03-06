/***************************************************************************
 *   Copyright (C) 2009 by Joris Guisson                                   *
 *   joris.guisson@gmail.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 ***************************************************************************/

#include "magnetdownloader.h"
#include <peer/peer.h>
#include <peer/peermanager.h>
#include <tracker/udptracker.h>
#include <tracker/httptracker.h>
#include <torrent/globals.h>
#include <dht/dhtbase.h>
#include <dht/dhtpeersource.h>
#include <kio/jobclasses.h>

#include "bcodec/bdecoder.h"
#include "bcodec/bnode.h"
#include "util/error.h"
#include <boost/concept_check.hpp>

namespace bt
{
	
	MagnetDownloader::MagnetDownloader(const bt::MagnetLink& mlink, QObject* parent)
		: QObject(parent),mlink(mlink),pman(0),dht_ps(0),tor(mlink.infoHash()),found(false)
	{
		dht::DHTBase & dht_table = Globals::instance().getDHT();
		connect(&dht_table,SIGNAL(started()),this,SLOT(dhtStarted()));
		connect(&dht_table,SIGNAL(stopped()),this,SLOT(dhtStopped()));
	}

	MagnetDownloader::~MagnetDownloader()
	{
		if (running())
			stop();
	}

	void MagnetDownloader::start()
	{
		if (running())
			return;

                
		if (!mlink.torrent().isEmpty()) 
		{
			KIO::StoredTransferJob *job = KIO::storedGet( QUrl(mlink.torrent()), KIO::NoReload, KIO::HideProgressInfo );
			connect(job,SIGNAL(result(KJob*)),this,SLOT(onTorrentDownloaded(KJob*)));
		}
		
		pman = new PeerManager(tor);
		connect(pman,SIGNAL(newPeer(Peer*)),this,SLOT(onNewPeer(Peer*)));
		
		foreach (const QUrl &url,mlink.trackers())
		{
			Tracker* tracker;
			if (url.scheme() == QLatin1String("udp"))
				tracker = new UDPTracker(url,this,tor.getPeerID(),0);
			else
				tracker = new HTTPTracker(url,this,tor.getPeerID(),0);
			trackers << tracker;
			connect(tracker,SIGNAL(peersReady(PeerSource*)),pman,SLOT(peerSourceReady(PeerSource*)));
			tracker->start();
		}
	
		dht::DHTBase & dht_table = Globals::instance().getDHT();
		if (dht_table.isRunning())
		{
			dht_ps = new dht::DHTPeerSource(dht_table,mlink.infoHash(),mlink.displayName());
			dht_ps->setRequestInterval(0); // Do not wait if the announce task finishes
			connect(dht_ps,SIGNAL(peersReady(PeerSource*)),pman,SLOT(peerSourceReady(PeerSource*)));
			dht_ps->start();
		}
		
		pman->start(false);
	}

	void MagnetDownloader::stop()
	{
		if (!running())
			return;
		
		foreach (Tracker* tracker,trackers)
		{
			tracker->stop();
			delete tracker;
		}
		trackers.clear();
		
		if (dht_ps)
		{
			dht_ps->stop();
			delete dht_ps;
			dht_ps = 0;
		}
		
		pman->stop();
		delete pman;
		pman = 0;
	}
	
	void MagnetDownloader::update()
	{
		if (pman)
			pman->update();
	}
	
	bool MagnetDownloader::running() const
	{
		return pman != 0;
	}

	Uint32 MagnetDownloader::numPeers() const
	{
		return pman ? pman->getNumConnectedPeers() : 0;
	}
		
	void MagnetDownloader::onNewPeer(Peer* p)
	{
		if (!p->getStats().extension_protocol)
		{
			// If the peer doesn't support the extension protocol, 
			// kill it
			p->kill();
		}
		else
		{
			connect(p,SIGNAL(metadataDownloaded(QByteArray)),this,SLOT(onMetadataDownloaded(QByteArray)));
		}
	}

	Uint64 MagnetDownloader::bytesDownloaded() const
	{
		return 0;
	}

	Uint64 MagnetDownloader::bytesUploaded() const
	{
		return 0;
	}

	Uint64 MagnetDownloader::bytesLeft() const
	{
		return 0;
	}
	
	bool MagnetDownloader::isPartialSeed() const
	{
		return false;
	}


	const bt::SHA1Hash& MagnetDownloader::infoHash() const
	{
		return mlink.infoHash();
	}

	void MagnetDownloader::onTorrentDownloaded(KJob* job)
	{
		if (!job) 
			return;
		
		KIO::StoredTransferJob* stj = qobject_cast<KIO::StoredTransferJob*>(job);
		if (job->error())
		{
			Out(SYS_GEN|LOG_DEBUG) << "Failed to download " << stj->url() << ": " << stj->errorString() << endl;
			return;
		}
		
		QByteArray data = stj->data();
		try
		{
			Torrent tor;
			tor.load(data,false);
			const TrackerTier* tier = tor.getTrackerList();
			while (tier)
			{
				mlink.tracker_urls.append(tier->urls);
				tier = tier->next;
			}
			onMetadataDownloaded(tor.getMetaData());
		} 
		catch (...) 
		{
			Out(SYS_GEN|LOG_NOTICE) << "Invalid torrent file from " << mlink.torrent() << endl;
		}
	}

	void MagnetDownloader::onMetadataDownloaded(const QByteArray& data)
	{
		if (found)
			return;
		
		bt::SHA1Hash hash = bt::SHA1Hash::generate((const Uint8*)data.data(),data.size());
		if (hash != mlink.infoHash())
		{
			Out(SYS_GEN|LOG_NOTICE) << "Metadata downloaded, but hash check failed" << endl;
			return;
		}
		
		found = true;
		Out(SYS_GEN|LOG_IMPORTANT) << "Metadata downloaded" << endl;
		foundMetadata(this,data);
		QTimer::singleShot(0,this,SLOT(stop()));
	}

	void MagnetDownloader::dhtStarted()
	{
		if (running() && !dht_ps)
		{
			dht::DHTBase & dht_table = Globals::instance().getDHT();
			dht_ps = new dht::DHTPeerSource(dht_table,mlink.infoHash(),mlink.displayName());
			dht_ps->setRequestInterval(0); // Do not wait if the announce task finishes
			connect(dht_ps,SIGNAL(peersReady(PeerSource*)),pman,SLOT(peerSourceReady(PeerSource*)));
			dht_ps->start();
		}
	}

	void MagnetDownloader::dhtStopped()
	{
		if (running() && dht_ps)
		{
			dht_ps->stop();
			delete dht_ps;
			dht_ps = 0;
		}
	}

}

