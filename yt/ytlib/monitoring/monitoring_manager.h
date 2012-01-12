#pragma once

#include <ytlib/ytree/ytree.h>
#include <ytlib/misc/periodic_invoker.h>

namespace NYT {
namespace NMonitoring {

////////////////////////////////////////////////////////////////////////////////

//! Provides monitoring info for registered systems in YSON format
/*!
 * \note Periodically updates info for all registered systems
 */
class TMonitoringManager
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr<TMonitoringManager> TPtr;

    //! Empty constructor.
    TMonitoringManager();

    //! Registers system for specified path.
    /*!
     * \param path      YPath for specified monitoring info.
     * \param producer  Monitoring info producer for the system.
     */
    void Register(const NYTree::TYPath& path, NYTree::TYsonProducer::TPtr producer);

    //! Unregisters system for specified path.
    /*!
     * \param path  YPath for specified monitoring info.
     */
    void Unregister(const NYTree::TYPath& path);

    //! Provides a root node containing info for all registered systems.
    /*!
     * \note Every update, the previous root expires and a new root is generated.
     */
    NYTree::INode::TPtr GetRoot() const;

    //! Starts periodic updates.
    void Start();

    //! Stops periodic updates.
    void Stop();

    //! Provides YSON producer for all monitoring info.
    /*!
     * \note Producer is sustained between updates.
     */
    NYTree::TYsonProducer::TPtr GetProducer();

private:
    typedef yhash<Stroka, NYTree::TYsonProducer::TPtr> TProducerMap;

    static const TDuration Period; // TODO: make configurable

    bool IsStarted;
    TPeriodicInvoker::TPtr PeriodicInvoker;

    //! Protects #MonitoringMap.
    TSpinLock SpinLock;
    TProducerMap MonitoringMap;

    NYTree::INode::TPtr Root;

    void Update();
    void Visit(NYTree::IYsonConsumer* consumer);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMonitoring
} // namespace NYT
