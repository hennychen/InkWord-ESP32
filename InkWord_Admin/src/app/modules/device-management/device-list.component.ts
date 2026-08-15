import { Component, OnInit, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatCardModule } from '@angular/material/card';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatDialog, MatDialogModule } from '@angular/material/dialog';
import { MatSnackBar } from '@angular/material/snack-bar';
import { DeviceApiService } from '../../core/api/device-api.service';
import { Device } from '../../core/models/models';
import { DeviceDetailComponent } from './device-detail.component';

/**
 * 设备管理列表 (A-09)：
 * 展示设备号、固件版本、在线状态（绿/红点）、最后活跃时间。
 * 点击设备行可展开详情。
 */
@Component({
  selector: 'app-device-list',
  standalone: true,
  imports: [
    CommonModule,
    MatCardModule,
    MatButtonModule,
    MatIconModule,
    MatProgressSpinnerModule,
    MatDialogModule,
  ],
  templateUrl: './device-list.component.html',
  styleUrl: './device-list.component.scss',
})
export class DeviceListComponent implements OnInit {
  private readonly deviceApi = inject(DeviceApiService);
  private readonly dialog = inject(MatDialog);
  private readonly snackBar = inject(MatSnackBar);

  readonly loading = signal(true);
  readonly devices = signal<Device[]>([]);

  ngOnInit(): void {
    this.loadDevices();
  }

  private loadDevices(): void {
    this.loading.set(true);
    this.deviceApi.list().subscribe({
      next: (res) => {
        this.devices.set(res.data ?? []);
        this.loading.set(false);
      },
      error: () => this.loading.set(false),
    });
  }

  /** 在线状态判断：最后心跳在 5 分钟内视为在线 */
  isOnline(device: Device): boolean {
    if (!device.lastHeartbeat) return false;
    const diff = Date.now() - new Date(device.lastHeartbeat).getTime();
    return diff < 5 * 60 * 1000;
  }

  /** 打开设备详情/命令面板 */
  openDetail(device: Device): void {
    const ref = this.dialog.open(DeviceDetailComponent, {
      width: '600px',
      data: device,
    });
    ref.afterClosed().subscribe(() => this.loadDevices());
  }
}
